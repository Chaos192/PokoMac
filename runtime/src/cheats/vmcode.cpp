// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
    template <class F>
    void flush(F&& f) {
        for (const Run& r : runs) f(r);
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
    std::lock_guard<std::mutex> work(work_mu_);
    run_ops_held();                                     // work queued by a toggle that found a pass doing its own
    {
        std::lock_guard<std::mutex> lk(mu_);
        end_pass_locked();
    }
    run_ops_held();
}

void Router::end_pass_locked() {
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
                else if (cur != want) ops_.push_back(write_op(w, want));
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
            ops_.push_back(write_op(w, want));
            caves.add(pl, w);
            c_.cave_words_written++;
        }
    }
    // the cave first: the branches into it go live later
    caves.flush([this](const CaveRuns::Run& r) { ops_.push_back(cave_op(r.cave_lo, r.cave_hi, r.lo, r.hi)); });
    if (!to_unpatch.empty()) {
        Op u;
        u.k = Op::K::Unpatch;
        u.words = to_unpatch;
        ops_.push_back(std::move(u));
        c_.words_reverted += to_unpatch.size();
    }
    // the cave words written back wait for the code reverted with them: settled once the unpatch above has been done (run_ops_held)
    for (const Held& c : caves_written_back) {
        CaveBack& b = caves_back_[c.addr];
        b.word = c;
        b.wait_for = to_unpatch;
    }
    if (!caves_written_back.empty() && to_unpatch.empty()) settle_caves_locked();
    if (!to_patch.empty()) {
        Op p;
        p.k = Op::K::Patch;
        p.patch = to_patch;
        ops_.push_back(std::move(p));
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
                if (sink_.read(w, &cur, 4) && cur == h->second.now) ops_.push_back(write_op(w, h->second.original));
            } else {
                code.push_back(w);
            }
        } else {
            cave.push_back(h->second);
        }
        held_.erase(h);
    }
    if (!code.empty()) {
        Op u;
        u.k = Op::K::Unpatch;
        u.words = code;
        ops_.push_back(std::move(u));
        c_.words_reverted += code.size();
    }
    for (Held& c : cave) {
        CaveBack& b = caves_back_[c.addr];
        b.word = c;
        b.wait_for.insert(b.wait_for.end(), code.begin(), code.end());
    }
    // with no code to wait for, the cave words go back with this work; else once the unpatch has been done (run_ops_held)
    if (!cave.empty() && code.empty()) settle_caves_locked();
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
            ops_.push_back(write_op(c.addr, c.original));
            runs.add(c.place, c.addr);
            c_.cave_words_restored++;
        }
        it = caves_back_.erase(it);
    }
    runs.flush([this](const CaveRuns::Run& r) { ops_.push_back(cave_op(r.cave_lo, r.cave_hi, r.lo, r.hi)); });
}

Router::Op Router::write_op(uint64_t w, uint32_t v) {
    Op o;
    o.k = Op::K::Write;
    o.a = w;
    o.v = v;
    return o;
}

Router::Op Router::cave_op(uint64_t cave_lo, uint64_t cave_hi, uint64_t lo, uint64_t hi) {
    Op o;
    o.k = Op::K::Cave;
    o.a = cave_lo;
    o.b = cave_hi;
    o.c = lo;
    o.d = hi;
    return o;
}

void Router::run_ops_held() {
    for (bool unpatched = false;; unpatched = false) {
        std::vector<Op> ops;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ops.swap(ops_);
        }
        if (ops.empty()) return;                        // (and what a toggle queued meanwhile is done too, before it returns)
        run_ops(ops, &unpatched);
        if (!unpatched) continue;
        // code went back: the cave words that waited for it go back now when it has settled (with no compile to wait for, at once)
        std::lock_guard<std::mutex> lk(mu_);
        if (!caves_back_.empty()) settle_caves_locked();
    }
}

void Router::run_ops(std::vector<Op>& ops, bool* unpatched) {
    for (Op& o : ops) {
        switch (o.k) {
            case Op::K::Write: sink_.write(o.a, &o.v, 4); break;
            case Op::K::Cave: sink_.cave_written(o.a, o.b, o.c, o.d); break;
            case Op::K::Unpatch:
                sink_.unpatch(o.words);
                *unpatched = true;
                break;
            case Op::K::Patch: {
                std::vector<std::string> errs;
                sink_.patch(o.patch, &errs);
                std::vector<size_t> refused;
                for (size_t i = 0; i < o.patch.size(); ++i)
                    if (i < errs.size() && !errs[i].empty()) refused.push_back(i);
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    c_.words_patched += o.patch.size() - refused.size();
                    c_.patch_failures += refused.size();
                    for (size_t i : refused)
                        if (auto it = held_.find(o.patch[i].first); it != held_.end()) it->second.plain = true;
                }
                // the patcher refused it (no on-demand compiler, a function too large ...): the bytes as before the router, said once
                for (size_t i : refused) {
                    sink_.write(o.patch[i].first, &o.patch[i].second, 4);
                    sink_.log("a cheat's code write at " + hex(o.patch[i].first) + " is not a live patch (" + errs[i] +
                              "): written as plain bytes, which the compiled game does not run");
                }
                break;
            }
        }
    }
}

void Router::run_ops_now_or_later() {
    // a toggle's work now when no pass is doing its own, else that pass does it first: the cheats overlay toggles from the render thread,
    // which must not wait for a compile a pass is waiting on
    std::unique_lock<std::mutex> work(work_mu_, std::try_to_lock);
    if (work.owns_lock()) run_ops_held();
}

void Router::entry_off(int entry) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        revoked_.insert(entry);
        std::vector<uint64_t> orphans;
        for (auto& [w, h] : held_)
            if (h.owners.erase(entry) && h.owners.empty()) orphans.push_back(w);
        restore_locked(orphans);
    }
    run_ops_now_or_later();
}

void Router::entry_on(int entry) {
    std::lock_guard<std::mutex> lk(mu_);
    revoked_.erase(entry);
}

void Router::reset() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        gen_++;
        std::vector<uint64_t> all;
        for (const auto& kv : held_) all.push_back(kv.first);
        restore_locked(all);
        revoked_.clear();
    }
    run_ops_now_or_later();
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

bool Router::applying(int entry) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [w, h] : held_)
        if (h.place.kind == Kind::Code && !h.plain && h.owners.count(entry) && sink_.applying(w)) return true;
    return false;
}

size_t Router::caves_waiting() const {
    std::lock_guard<std::mutex> lk(mu_);
    return caves_back_.size();
}

}  // namespace nxrt::cheats::vmcode
