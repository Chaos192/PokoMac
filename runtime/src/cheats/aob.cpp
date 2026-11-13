// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Array-of-bytes patterns and signatures (aob.h).
#include "aob.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>

namespace nxrt::cheats {
namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool is_wild(char c) { return c == '?' || c == '*'; }

}  // namespace

bool aob_parse(const std::string& text, AobPattern* out, std::string* err) {
    AobPattern p;
    // collect two-character tokens: split on whitespace; if a token is longer than 2, cut it into 2-char pieces (a no-space run).
    std::vector<std::string> toks;
    size_t i = 0;
    const size_t n = text.size();
    auto push_run = [&](const std::string& run) -> bool {
        if (run.size() % 2 != 0) { if (err) *err = "a byte token must be two characters: '" + run + "'"; return false; }
        for (size_t k = 0; k < run.size(); k += 2) toks.push_back(run.substr(k, 2));
        return true;
    };
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        size_t j = i;
        while (j < n && !std::isspace(static_cast<unsigned char>(text[j]))) ++j;
        if (j > i) { if (!push_run(text.substr(i, j - i))) return false; }
        i = j;
    }
    if (toks.empty()) { if (err) *err = "the pattern is empty"; return false; }
    for (const std::string& tk : toks) {
        const char hi = tk[0], lo = tk[1];
        uint8_t val = 0, mask = 0;
        if (is_wild(hi)) { /* mask high nibble out */ }
        else { const int h = hex_nibble(hi); if (h < 0) { if (err) *err = "not a hex digit or wildcard: '" + std::string(1, hi) + "'"; return false; } val |= h << 4; mask |= 0xF0; }
        if (is_wild(lo)) { /* mask low nibble out */ }
        else { const int l = hex_nibble(lo); if (l < 0) { if (err) *err = "not a hex digit or wildcard: '" + std::string(1, lo) + "'"; return false; } val |= l; mask |= 0x0F; }
        p.val.push_back(val);
        p.mask.push_back(mask);
    }
    *out = std::move(p);
    return true;
}

std::string aob_text(const AobPattern& p) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < p.val.size(); ++i) {
        if (i) s += ' ';
        const uint8_t m = p.mask[i], v = p.val[i];
        const char hi = (m & 0xF0) ? "0123456789ABCDEF"[(v >> 4) & 0xF] : '?';
        const char lo = (m & 0x0F) ? "0123456789ABCDEF"[v & 0xF] : '?';
        b[0] = hi; b[1] = lo; b[2] = 0;
        s += b;
    }
    return s;
}

AobPattern aob_of_bytes(const uint8_t* bytes, size_t n) {
    AobPattern p;
    p.val.assign(bytes, bytes + n);
    p.mask.assign(n, 0xFF);
    return p;
}

std::vector<AobHit> aob_scan(MemSource& src, const AobPattern& pat, const RegionFilter& filter, size_t limit, ScanProgress* prog) {
    std::vector<AobHit> out;
    if (pat.empty()) return out;
    if (limit == 0) limit = 100000;
    const size_t plen = pat.size();
    auto regions = src.regions(filter);
    uint64_t total = 0;
    for (const auto& r : regions) total += r.size;
    if (prog) { prog->bytes_total = total; prog->bytes_done = 0; prog->results = 0; prog->running = true; }

    std::mutex mu;
    std::atomic<size_t> ri{0};
    std::atomic<bool> full{false};
    auto work = [&] {
        std::vector<uint8_t> buf;
        for (;;) {
            const size_t i = ri.fetch_add(1);
            if (i >= regions.size() || full.load() || (prog && prog->cancel.load())) break;
            const ScanRegion& r = regions[i];
            const size_t chunk = 1 << 20;
            // overlap chunks by plen-1 so a match across a chunk boundary is found
            for (uint64_t off = 0; off < r.size; off += chunk) {
                if (full.load() || (prog && prog->cancel.load())) break;
                const uint64_t want = std::min<uint64_t>(chunk + plen - 1, r.size - off);
                const size_t n = static_cast<size_t>(want);
                buf.resize(n);
                if (!src.read(r.addr + off, buf.data(), n)) { if (prog) prog->bytes_done += std::min<uint64_t>(chunk, r.size - off); continue; }
                std::vector<uint64_t> hits;
                if (n >= plen)
                    for (size_t p = 0; p + plen <= n; ++p)
                        if (pat.matches(buf.data() + p)) hits.push_back(r.addr + off + p);
                if (!hits.empty()) {
                    std::lock_guard<std::mutex> lk(mu);
                    for (uint64_t h : hits) {
                        if (out.size() >= limit) { full = true; break; }
                        out.push_back({h});
                    }
                    if (prog) prog->results = out.size();
                }
                if (prog) prog->bytes_done += std::min<uint64_t>(chunk, r.size - off);
            }
        }
    };
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 4; if (hc > 8) hc = 8;
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < hc; ++k) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    if (prog) prog->running = false;
    std::sort(out.begin(), out.end(), [](const AobHit& a, const AobHit& b) { return a.addr < b.addr; });
    return out;
}

size_t aob_count_in_range(MemSource& src, const AobPattern& pat, uint64_t lo, uint64_t hi, size_t stop_at) {
    if (pat.empty() || hi <= lo) return 0;
    const size_t plen = pat.size();
    size_t count = 0;
    std::vector<uint8_t> buf;
    const uint64_t chunk = 1 << 20;
    for (uint64_t off = lo; off < hi && count < stop_at;) {
        const uint64_t want = std::min<uint64_t>(chunk + plen - 1, hi - off);
        const size_t n = static_cast<size_t>(want);
        buf.resize(n);
        if (!src.read(off, buf.data(), n)) { off += chunk; continue; }
        if (n >= plen)
            for (size_t p = 0; p + plen <= n && count < stop_at; ++p)
                if (pat.matches(buf.data() + p)) ++count;
        off += chunk;
    }
    return count;
}

Signature aob_make_signature(MemSource& src, uint64_t addr, uint64_t range_lo, uint64_t range_hi,
                             const std::vector<size_t>& wildcard, size_t min_len, size_t max_len) {
    Signature sig;
    std::vector<uint8_t> bytes(max_len);
    size_t have = 0;
    for (; have < max_len; ++have)
        if (!src.read(addr + have, bytes.data() + have, 1)) break;
    if (have < min_len) { sig.matches = 0; return sig; }

    auto make = [&](size_t len) {
        AobPattern p = aob_of_bytes(bytes.data(), len);
        for (size_t w : wildcard) if (w < len) { p.mask[w] = 0x00; p.val[w] = 0x00; }   // the disassembler's relocated / immediate bytes
        return p;
    };
    // grow from min_len until unique or out of bytes
    for (size_t len = std::min(min_len, have); len <= have; ++len) {
        AobPattern p = make(len);
        const size_t count = aob_count_in_range(src, p, range_lo, range_hi, 3);
        if (count <= 1) {
            sig.pattern = std::move(p);
            sig.matches = count;
            sig.delta = 0;
            return sig;
        }
        if (len == have) {
            sig.pattern = std::move(p);
            sig.matches = count;
            sig.delta = 0;
            return sig;
        }
    }
    sig.pattern = make(have);
    sig.matches = aob_count_in_range(src, sig.pattern, range_lo, range_hi, 100);
    return sig;
}

}  // namespace nxrt::cheats
