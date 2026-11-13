// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The cheat VM's write router (vmcode.h).
#include "vmcode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nxrt::cheats::vmcode {
namespace {
std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// Contiguous cave words of one cave, for one cave_written call each.
struct CaveRuns {
    struct Run { uint64_t cave_lo, cave_hi, lo, hi; };
    std::vector<Run> runs;
    void add(const Place& pl, uint64_t w) {
        if (!runs.empty() && runs.back().cave_lo == pl.lo && runs.back().hi == w) runs.back().hi = w + 4;
        else runs.push_back({pl.lo, pl.hi, w, w + 4});
    }
    void flush(Sink& s) {
        for (const Run& r : runs) s.cave_written(r.cave_lo, r.cave_hi, r.lo, r.hi);
        runs.clear();
    }
};
}  // namespace

void Router::begin_pass() {
    pend_.clear();
    pend_words_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        pass_gen_ = gen_;
    }
    sink_.refresh();
}

bool Router::write(uint64_t addr, const void* src, size_t n, int entry) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    bool ok = true, code = false, cave = false;
    size_t i = 0;
    while (i < n) {
        const uint64_t a = addr + i;
        const uint64_t w = a & ~3ull;
        const size_t take = std::min<size_t>(n - i, static_cast<size_t>(w + 4 - a));   // to the end of this word
        const Place pl = sink_.place(w);
        if (pl.kind == Kind::Data) {
            if (!sink_.write(a, p + i, take)) ok = false;   // not code the build lifted: a plain write, as before
        } else {
            PendWord& pw = pend_words_[w];
            pw.place = pl;
            auto& mine = pw.by[entry];
            for (size_t k = 0; k < take; ++k) {
                const unsigned at = static_cast<unsigned>(a + k - w);
                pend_[a + k] = p[i + k];
                mine.first |= static_cast<uint8_t>(1u << at);
                mine.second = (mine.second & ~(0xFFu << (8 * at))) | (uint32_t(p[i + k]) << (8 * at));
            }
            (pl.kind == Kind::Code ? code : cave) = true;
        }
        i += take;
    }
    if (code || cave) {
        std::lock_guard<std::mutex> lk(mu_);
        if (code) c_.code_writes++;
        if (cave) c_.cave_writes++;
    }
    return ok;
}

bool Router::read(uint64_t addr, void* dst, size_t n) {
    const bool ok = sink_.read(addr, dst, n);
    if (pend_.empty()) return ok;
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (auto it = pend_.lower_bound(addr); it != pend_.end() && it->first < addr + n; ++it) d[it->first - addr] = it->second;
    return ok;
}

void Router::end_pass() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!caves_back_.empty()) settle_caves_locked();
    if (pend_words_.empty() || pass_gen_ != gen_) {   // nothing written to code, or the file was reloaded while the pass ran
        pend_.clear();
        pend_words_.clear();
        revoked_.clear();
        return;
    }
    c_.passes_with_code++;
    std::vector<std::pair<uint64_t, uint32_t>> to_patch;
    std::vector<uint64_t> to_unpatch;
    std::vector<Held> caves_written_back;                   // cave words the cheats wrote back to their original bytes
    CaveRuns caves;
    for (const auto& [w, pw] : pend_words_) {
        const Place& pl = pw.place;
        uint32_t cur = 0;
        if (!sink_.read(w, &cur, 4)) {
            sink_.log("a cheat wrote code at " + hex(w) + ", which cannot be read: left as it is");
            continue;
        }
        uint8_t b[4];
        std::memcpy(b, &cur, 4);
        for (int k = 0; k < 4; ++k)
            if (auto it = pend_.find(w + k); it != pend_.end()) b[k] = it->second;
        uint32_t want = 0;
        std::memcpy(&want, b, 4);
        // the cheats whose last write here is what the word now holds own it (a cheat that writes the original bytes before another
        // writes its patch over them does not); when none alone made it so, every cheat that wrote here does
        std::set<int> made, wrote;
        for (const auto& [e, mb] : pw.by) {
            if (e < 0) continue;
            wrote.insert(e);
            uint32_t mask = 0;
            for (int k = 0; k < 4; ++k)
                if (mb.first & (1u << k)) mask |= 0xFFu << (8 * k);
            if ((mb.second & mask) == (want & mask)) made.insert(e);
        }
        const std::set<int>& by = made.empty() ? wrote : made;
        std::set<int> writers;
        for (int e : by)
            if (!revoked_.count(e)) writers.insert(e);
        if (!by.empty() && writers.empty()) continue;      // the cheats that wrote it were turned off while the pass ran: the next pass decides
        // a cave word waiting to go back is the cheats' again (turned on again before it went back)
        if (auto cb = caves_back_.find(w); cb != caves_back_.end()) {
            if (want == cb->second.word.original) continue;      // already on its way back
            held_[w] = cb->second.word;
            held_[w].owners.clear();
            caves_back_.erase(cb);
        }
        auto h = held_.find(w);
        const uint32_t original = h != held_.end() ? h->second.original : cur;
        if (want == original) {
            if (h != held_.end()) {                         // written back to its original bytes: the cheats' change goes
                if (pl.kind == Kind::Code && !h->second.plain) to_unpatch.push_back(w);
                else if (pl.kind == Kind::Cave) caves_written_back.push_back(h->second);   // once the code reverted with it settles
                else if (cur != want) sink_.write(w, &want, 4);
                held_.erase(h);
            }
            continue;
        }
        Held& rec = held_[w];
        if (h == held_.end()) {
            rec.addr = w;
            rec.place = pl;
            rec.original = original;
        }
        rec.owners.insert(writers.begin(), writers.end());
        rec.now = want;
        if (want == cur) continue;                          // already so (the patch from an earlier pass)
        if (pl.kind == Kind::Code) {
            to_patch.push_back({w, want});
        } else {
            sink_.write(w, &want, 4);
            caves.add(pl, w);
            c_.cave_words_written++;
        }
    }
    caves.flush(sink_);                                     // the cave first: the branches into it go live later
    if (!to_unpatch.empty()) {
        sink_.unpatch(to_unpatch);
        c_.words_reverted += to_unpatch.size();
    }
    for (const Held& c : caves_written_back) {
        CaveBack& b = caves_back_[c.addr];
        b.word = c;
        b.wait_for = to_unpatch;
    }
    if (!caves_written_back.empty()) settle_caves_locked();
    if (!to_patch.empty()) {
        std::vector<std::string> errs;
        sink_.patch(to_patch, &errs);
        for (size_t i = 0; i < to_patch.size(); ++i) {
            if (i < errs.size() && !errs[i].empty()) {
                // the patcher refused it (no on-demand compiler, a function too large ...): the bytes as before the router, said once
                c_.patch_failures++;
                sink_.write(to_patch[i].first, &to_patch[i].second, 4);
                if (auto it = held_.find(to_patch[i].first); it != held_.end()) it->second.plain = true;
                sink_.log("a cheat's code write at " + hex(to_patch[i].first) + " is not a live patch (" + errs[i] +
                          "): written as plain bytes, which the compiled game does not run");
            } else {
                c_.words_patched++;
            }
        }
    }
    pend_.clear();
    pend_words_.clear();
    revoked_.clear();
}

void Router::restore_locked(const std::vector<uint64_t>& words) {
    std::vector<uint64_t> code;
    std::vector<Held> cave;
    for (uint64_t w : words) {
        auto h = held_.find(w);
        if (h == held_.end()) continue;
        if (h->second.place.kind == Kind::Code) {
            if (h->second.plain) {
                uint32_t cur = 0;
                if (sink_.read(w, &cur, 4) && cur == h->second.now) sink_.write(w, &h->second.original, 4);
            } else {
                code.push_back(w);
            }
        } else {
            cave.push_back(h->second);
        }
        held_.erase(h);
    }
    if (!code.empty()) {
        sink_.unpatch(code);
        c_.words_reverted += code.size();
    }
    for (Held& c : cave) {
        CaveBack& b = caves_back_[c.addr];
        b.word = c;
        b.wait_for.insert(b.wait_for.end(), code.begin(), code.end());
    }
    if (!cave.empty()) settle_caves_locked();
}

void Router::settle_caves_locked() {
    CaveRuns runs;
    for (auto it = caves_back_.begin(); it != caves_back_.end();) {
        bool ready = true;
        for (uint64_t w : it->second.wait_for)
            if (!sink_.settled(w)) { ready = false; break; }
        if (!ready) { ++it; continue; }
        const Held& c = it->second.word;
        uint32_t cur = 0;
        if (sink_.read(c.addr, &cur, 4) && cur == c.now) {   // still the cheats' bytes (nothing else wrote there since)
            sink_.write(c.addr, &c.original, 4);
            runs.add(c.place, c.addr);
            c_.cave_words_restored++;
        }
        it = caves_back_.erase(it);
    }
    runs.flush(sink_);
}

void Router::entry_off(int entry) {
    std::lock_guard<std::mutex> lk(mu_);
    revoked_.insert(entry);
    std::vector<uint64_t> orphans;
    for (auto& [w, h] : held_)
        if (h.owners.erase(entry) && h.owners.empty()) orphans.push_back(w);
    restore_locked(orphans);
}

void Router::entry_on(int entry) {
    std::lock_guard<std::mutex> lk(mu_);
    revoked_.erase(entry);
}

void Router::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    gen_++;
    std::vector<uint64_t> all;
    for (const auto& kv : held_) all.push_back(kv.first);
    restore_locked(all);
    revoked_.clear();
}

Counters Router::counters() const {
    std::lock_guard<std::mutex> lk(mu_);
    return c_;
}

std::vector<Held> Router::held() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Held> out;
    for (const auto& kv : held_) out.push_back(kv.second);
    return out;
}

size_t Router::caves_waiting() const {
    std::lock_guard<std::mutex> lk(mu_);
    return caves_back_.size();
}

}  // namespace nxrt::cheats::vmcode
