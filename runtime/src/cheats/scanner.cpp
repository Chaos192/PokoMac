// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Memory scanner (scanner.h).
#include "scanner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "aob.h"
#include "gmem.h"

namespace nxrt::cheats {

size_t type_size(ScanType t) {
    switch (t) {
        case ScanType::U8: return 1;
        case ScanType::U16: return 2;
        case ScanType::U32: case ScanType::F32: return 4;
        case ScanType::U64: case ScanType::F64: return 8;
    }
    return 4;
}
const char* type_name(ScanType t) {
    switch (t) {
        case ScanType::U8: return "byte";
        case ScanType::U16: return "2 bytes";
        case ScanType::U32: return "4 bytes";
        case ScanType::U64: return "8 bytes";
        case ScanType::F32: return "float";
        case ScanType::F64: return "double";
    }
    return "?";
}
bool type_is_float(ScanType t) { return t == ScanType::F32 || t == ScanType::F64; }

double Value::as_double() const {
    if (type == ScanType::F32) { float f; std::memcpy(&f, &bits, 4); return f; }
    if (type == ScanType::F64) { double d; std::memcpy(&d, &bits, 8); return d; }
    return static_cast<double>(as_int());
}
int64_t Value::as_int() const {
    switch (type) {
        case ScanType::U8: return static_cast<int8_t>(bits & 0xFF);
        case ScanType::U16: return static_cast<int16_t>(bits & 0xFFFF);
        case ScanType::U32: return static_cast<int32_t>(bits & 0xFFFFFFFF);
        case ScanType::U64: return static_cast<int64_t>(bits);
        default: return static_cast<int64_t>(as_double());
    }
}
void Value::set_double(double d) {
    if (type == ScanType::F32) { float f = static_cast<float>(d); std::memcpy(&bits, &f, 4); }
    else { std::memcpy(&bits, &d, 8); }
}
std::string Value::text() const {
    char b[64];
    if (type == ScanType::F32 || type == ScanType::F64) std::snprintf(b, sizeof b, "%g", as_double());
    else std::snprintf(b, sizeof b, "%lld", static_cast<long long>(as_int()));
    return b;
}

bool parse_value(ScanType t, const std::string& s, Value* out) {
    Value v;
    v.type = t;
    if (s.empty()) return false;
    try {
        if (t == ScanType::F32 || t == ScanType::F64) {
            v.set_double(std::stod(s));
        } else {
            const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
            const long long n = std::stoll(s, nullptr, hex ? 16 : 10);
            v.bits = static_cast<uint64_t>(n);
        }
    } catch (...) {
        return false;
    }
    *out = v;
    return true;
}

namespace {

uint64_t load_bits(const uint8_t* p, ScanType t) {
    uint64_t v = 0;
    std::memcpy(&v, p, type_size(t));
    return v;
}

// A reference typed as a positive number above the type's signed range (200 for a byte, 40000 for 2 bytes, 3000000000 for 4 bytes)
// asks about the value read unsigned; any other reference compares signed, as the values are shown (Value::text). Live on Wonder (w22):
// "smaller than 200" on a byte holding 113 found nothing, because 200 was taken as the signed byte -56.
bool unsigned_ref(ScanType t, uint64_t ref) {
    const size_t n = type_size(t);
    if (type_is_float(t) || n >= 8) return false;
    return (ref >> (8 * n)) == 0 && ((ref >> (8 * n - 1)) & 1) != 0;
}
int64_t int_of(ScanType t, uint64_t bits, bool uns) {
    const size_t n = type_size(t);
    if (!uns || n >= 8) return Value{t, bits}.as_int();
    return static_cast<int64_t>(bits & ((uint64_t{1} << (8 * n)) - 1));
}
// The tolerance of a float comparison against a typed value: a few units in the last place of the type (relative), so a typed 1000.5
// matches the float 1000.5 and not 1000.21. (It was 1e-3 relative for floats: live on Wonder, w22, "exact 1000.5" also kept 999.586 and
// 1000.21, 4,043 results, and "changed" missed a float that went from 1012.5 to 1013.5.)
double float_tol(ScanType t, double scale) { return (t == ScanType::F32 ? 1e-6 : 1e-12) * (1.0 + scale); }
// One unit in the last place of the type at magnitude m.
double ulp_of(ScanType t, double m) {
    m = std::fabs(m);
    if (t == ScanType::F32) {
        const float f = static_cast<float>(m);
        return static_cast<double>(std::nextafter(f, std::numeric_limits<float>::infinity())) - static_cast<double>(f);
    }
    return std::nextafter(m, std::numeric_limits<double>::infinity()) - m;
}
// "Increased by" / "decreased by" a float: the change d must go the asked way and come within the delta's tolerance or two units in the
// last place of the values (a float at 6.6e8 moves in steps of 64: it cannot change by exactly 10). Live on Wonder (w23), a tolerance
// scaled by the values' size kept every unchanged large float as "decreased by 10" (2,000,000 results, the cap).
bool float_by(ScanType t, double d, double x, double p, double lo) {
    if (lo > 0 ? !(d > 0) : lo < 0 ? !(d < 0) : d != 0) return false;
    return std::fabs(d - lo) <= std::max(float_tol(t, std::fabs(lo)), 2.0 * ulp_of(t, std::max(std::fabs(x), std::fabs(p))));
}

// A typed comparison against one or two reference values, with a float tolerance for exact matches.
bool cmp_scalar(ScanType t, uint64_t cur, FirstScan how, uint64_t a, uint64_t b) {
    if (type_is_float(t)) {
        Value cv{t, cur}, av{t, a}, bv{t, b};
        const double x = cv.as_double(), lo = av.as_double(), hi = bv.as_double();
        if (!std::isfinite(x)) return false;
        switch (how) {
            case FirstScan::Exact: return std::fabs(x - lo) <= float_tol(t, std::fabs(lo));
            case FirstScan::Between: return x >= std::min(lo, hi) && x <= std::max(lo, hi);
            case FirstScan::Greater: return x > lo;
            case FirstScan::Less: return x < lo;
            case FirstScan::GreaterOrEqual: return x >= lo;
            case FirstScan::LessOrEqual: return x <= lo;
            case FirstScan::Unknown: return true;
        }
        return false;
    }
    const bool uns = unsigned_ref(t, a) || (how == FirstScan::Between && unsigned_ref(t, b));
    const int64_t x = int_of(t, cur, uns), lo = int_of(t, a, uns), hi = int_of(t, b, uns);
    switch (how) {
        case FirstScan::Exact: return x == lo;
        case FirstScan::Between: return x >= std::min(lo, hi) && x <= std::max(lo, hi);
        case FirstScan::Greater: return x > lo;
        case FirstScan::Less: return x < lo;
        case FirstScan::GreaterOrEqual: return x >= lo;
        case FirstScan::LessOrEqual: return x <= lo;
        case FirstScan::Unknown: return true;
    }
    return false;
}

bool cmp_next(ScanType t, uint64_t cur, uint64_t prev, uint64_t first, NextScan how, uint64_t a, uint64_t b) {
    if (type_is_float(t)) {
        Value cv{t, cur}, pv{t, prev}, fv{t, first}, av{t, a}, bv{t, b};
        const double x = cv.as_double(), p = pv.as_double(), f = fv.as_double(), lo = av.as_double(), hi = bv.as_double();
        if (!std::isfinite(x)) return false;
        switch (how) {
            case NextScan::Exact: return std::fabs(x - lo) <= float_tol(t, std::fabs(lo));
            case NextScan::Changed: return x != p;              // memory changed or it did not: no tolerance
            case NextScan::Unchanged: return x == p;
            case NextScan::Increased: return x > p;
            case NextScan::Decreased: return x < p;
            case NextScan::IncreasedBy: return float_by(t, x - p, x, p, lo);
            case NextScan::DecreasedBy: return float_by(t, p - x, x, p, lo);
            case NextScan::Between: return x >= std::min(lo, hi) && x <= std::max(lo, hi);
            case NextScan::Greater: return x > lo;
            case NextScan::Less: return x < lo;
            case NextScan::SameAsFirst: return x == f;
            case NextScan::Unknown: return true;
        }
        return false;
    }
    const bool uns = (how == NextScan::Exact || how == NextScan::Between || how == NextScan::Greater || how == NextScan::Less) &&
                     (unsigned_ref(t, a) || (how == NextScan::Between && unsigned_ref(t, b)));
    const int64_t x = int_of(t, cur, uns), p = int_of(t, prev, uns), f = int_of(t, first, uns), lo = int_of(t, a, uns), hi = int_of(t, b, uns);
    switch (how) {
        case NextScan::Exact: return x == lo;
        case NextScan::Changed: return x != p;
        case NextScan::Unchanged: return x == p;
        case NextScan::Increased: return x > p;
        case NextScan::Decreased: return x < p;
        case NextScan::IncreasedBy: return x - p == lo;
        case NextScan::DecreasedBy: return p - x == lo;
        case NextScan::Between: return x >= std::min(lo, hi) && x <= std::max(lo, hi);
        case NextScan::Greater: return x > lo;
        case NextScan::Less: return x < lo;
        case NextScan::SameAsFirst: return x == f;
        case NextScan::Unknown: return true;
    }
    return false;
}

unsigned worker_count(unsigned want) {
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 4;
    if (hc > 8) hc = 8;                 // keep the game responsive on a busy machine
    return want ? std::min(want, hc) : hc;
}

// A live source over gmem.
class GmemSource : public MemSource {
public:
    std::vector<ScanRegion> regions(const RegionFilter& f) override {
        std::vector<ScanRegion> out;
        for (const auto& r : gmem::regions()) {
            const bool is_main = r.module == "main";
            const std::string k = r.kind;
            bool keep = false;
            if (k == "heap") keep = f.heap;
            else if (k == "alias") keep = f.alias;
            else if (k == "stack") keep = f.stack;
            else if (k == "code") keep = f.code;
            else if (k == "data") keep = is_main ? f.main : f.other;   // a module's data/bss: main's by the main filter, others by "other"
            else keep = f.other;                                        // tls, shared, anything else
            if (!keep) continue;
            if (f.writable_only && !(r.perm & 2)) continue;
            ScanRegion sr{r.addr, r.size, r.perm, r.kind};
            if (f.lo || f.hi) {
                const uint64_t lo = std::max(sr.addr, f.lo), hi = std::min(sr.addr + sr.size, f.hi ? f.hi : sr.addr + sr.size);
                if (lo >= hi) continue;
                sr.addr = lo;
                sr.size = hi - lo;
            }
            out.push_back(sr);
        }
        return out;
    }
    bool read(uint64_t addr, void* dst, size_t n) override { return gmem::read(addr, dst, n); }
};

}  // namespace

std::shared_ptr<MemSource> live_source() { return std::make_shared<GmemSource>(); }

struct Scanner::Snapshot {
    struct Chunk { uint64_t addr; std::vector<uint8_t> bytes; };
    std::vector<Chunk> chunks;
};

const char* kind_name(ScanKind k) {
    switch (k) {
        case ScanKind::Numeric: return "value";
        case ScanKind::AllNumeric: return "all numeric types";
        case ScanKind::Utf8: return "text (UTF-8)";
        case ScanKind::Utf16: return "text (UTF-16)";
        case ScanKind::Aob: return "array of bytes";
    }
    return "?";
}

namespace {
constexpr ScanType kAllTypes[] = {ScanType::U8, ScanType::U16, ScanType::U32, ScanType::U64, ScanType::F32, ScanType::F64};

uint64_t hash_bytes(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

bool pat_match(const uint8_t* p, const std::vector<uint8_t>& val, const std::vector<uint8_t>& mask) {
    for (size_t i = 0; i < val.size(); ++i)
        if ((p[i] & mask[i]) != val[i]) return false;
    return true;
}

bool is_ascii_letter(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// The pattern a text / AOB scan looks for: UTF-8 bytes, UTF-16LE code units (from UTF-8 text), or a parsed AOB. With ignore_case an
// ASCII letter's case bit (0x20) is masked out.
bool build_pattern(const ScanOptions& opt, std::vector<uint8_t>* val, std::vector<uint8_t>* mask) {
    val->clear();
    mask->clear();
    auto push = [&](uint8_t b, bool letter) {
        const uint8_t m = (letter && opt.ignore_case) ? 0xDF : 0xFF;
        val->push_back(b & m);
        mask->push_back(m);
    };
    if (opt.kind == ScanKind::Aob) {
        AobPattern ap;
        if (!aob_parse(opt.text, &ap)) return false;
        *val = ap.val;
        *mask = ap.mask;
        return !val->empty();
    }
    if (opt.text.empty()) return false;
    if (opt.kind == ScanKind::Utf8) {
        for (unsigned char ch : opt.text) push(ch, is_ascii_letter(ch));
        return true;
    }
    // UTF-8 -> UTF-16LE
    const std::string& t = opt.text;
    auto at = [&](size_t k) { return k < t.size() ? static_cast<uint32_t>(static_cast<unsigned char>(t[k])) & 0x3F : 0u; };
    for (size_t i = 0; i < t.size();) {
        uint32_t cp = static_cast<unsigned char>(t[i]);
        size_t n = 1;
        if (cp >= 0xF0) { cp = ((cp & 7) << 18) | (at(i + 1) << 12) | (at(i + 2) << 6) | at(i + 3); n = 4; }
        else if (cp >= 0xE0) { cp = ((cp & 0xF) << 12) | (at(i + 1) << 6) | at(i + 2); n = 3; }
        else if (cp >= 0xC0) { cp = ((cp & 0x1F) << 6) | at(i + 1); n = 2; }
        i += n;
        auto unit = [&](uint32_t u) { push(static_cast<uint8_t>(u & 0xFF), is_ascii_letter(u)); push(static_cast<uint8_t>(u >> 8), false); };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    return !val->empty();
}

bool is_pattern_kind(ScanKind k) { return k == ScanKind::Utf8 || k == ScanKind::Utf16 || k == ScanKind::Aob; }
}  // namespace

Scanner::Scanner(std::shared_ptr<MemSource> src) : src_(std::move(src)) {}

size_t Scanner::result_count() const { return is_unknown_ ? 0 : addrs_.size(); }

void Scanner::clear() {
    addrs_.clear();
    first_bits_.clear();
    last_bits_.clear();
    types_.clear();
    snap_.reset();
    is_unknown_ = false;
    capped_ = false;
}

void Scanner::push_undo_state() {
    UndoState u;
    u.addrs = addrs_;
    u.first = first_bits_;
    u.last = last_bits_;
    u.types = types_;
    u.pat_val = pat_val_;
    u.pat_mask = pat_mask_;
    u.snap = snap_;
    u.unknown = is_unknown_;
    u.kind = kind_;
    u.type = opt_.type;
    u.pat_len = pat_len_;
    undo_.push_back(std::move(u));
    if (undo_.size() > 4) undo_.erase(undo_.begin());    // keep the last few scans' worth
}

void Scanner::undo() {
    if (undo_.empty()) return;
    UndoState& u = undo_.back();
    addrs_ = std::move(u.addrs);
    first_bits_ = std::move(u.first);
    last_bits_ = std::move(u.last);
    types_ = std::move(u.types);
    pat_val_ = std::move(u.pat_val);
    pat_mask_ = std::move(u.pat_mask);
    snap_ = std::move(u.snap);
    is_unknown_ = u.unknown;
    kind_ = u.kind;
    opt_.type = u.type;
    pat_len_ = u.pat_len;
    undo_.pop_back();
}

void Scanner::first_scan(const ScanOptions& opt_in, FirstScan cmp, const Value& v1, const Value& v2, ScanProgress& prog) {
    push_undo_state();
    ScanOptions opt = opt_in;
    if (opt.all_numeric) opt.kind = ScanKind::AllNumeric;
    opt_ = opt;
    kind_ = opt.kind;
    addrs_.clear();
    first_bits_.clear();
    last_bits_.clear();
    types_.clear();
    snap_.reset();
    capped_ = false;
    pat_len_ = 0;
    const bool pattern = is_pattern_kind(kind_);
    if (pattern) {
        if (!build_pattern(opt, &pat_val_, &pat_mask_)) {
            prog.bytes_total = 0;
            prog.bytes_done = 0;
            prog.results = 0;
            prog.running = false;
            return;
        }
        pat_len_ = pat_val_.size();
        cmp = FirstScan::Exact;
    }
    const ScanType t = opt.type;
    const size_t ts = pattern ? pat_len_ : type_size(t);
    // the step: values at their alignment (fast) or every byte; a text or pattern at every byte (UTF-16 at even bytes when fast)
    unsigned step = opt.alignment ? opt.alignment : (opt.fast ? static_cast<unsigned>(ts) : 1);
    if (pattern) step = opt.alignment ? opt.alignment : (kind_ == ScanKind::Utf16 && opt.fast ? 2u : 1u);
    if (step == 0) step = 1;

    auto regions = src_->regions(opt.filter);
    uint64_t total = 0;
    for (const auto& r : regions) total += r.size;
    prog.bytes_total = total;
    prog.bytes_done = 0;
    prog.results = 0;
    prog.running = true;

    is_unknown_ = cmp == FirstScan::Unknown && !pattern;
    if (is_unknown_) snap_ = std::make_shared<Snapshot>();   // the first next scan turns the snapshot into results

    // all numeric types: the reference text parsed as each type (a type it does not parse as is skipped), so "100" also finds 100.0f
    struct TypeRef { ScanType t; Value a, b; };
    std::vector<TypeRef> refs;
    if (kind_ == ScanKind::AllNumeric) {
        const std::string t1 = opt.text.empty() ? v1.text() : opt.text;
        const std::string t2 = v2.text();
        for (ScanType at : kAllTypes) {
            TypeRef r{at, {}, {}};
            bool ok = true;
            if (cmp != FirstScan::Unknown) {
                ok = parse_value(at, t1, &r.a);
                if (cmp == FirstScan::Between) ok = ok && parse_value(at, t2, &r.b);
                // a value that does not fit the type is not a match of that type (300 is no byte)
                if (ok && !type_is_float(at) && type_size(at) < 8) {
                    const int64_t want = std::strtoll(t1.c_str(), nullptr, 0);
                    const int64_t lim = 1ll << (8 * type_size(at));
                    if (!(t1.find('.') == std::string::npos && want > -lim / 2 - 1 && want < lim)) ok = false;
                }
            }
            if (ok) refs.push_back(r);
        }
    }

    std::mutex out_mu;
    std::atomic<size_t> ri{0};
    const unsigned nthreads = worker_count(opt.threads);
    const size_t overlap = ts > 1 ? ts - 1 : 0;   // a text or pattern may straddle two chunks
    auto work = [&] {
        std::vector<uint8_t> buf;
        for (;;) {
            const size_t i = ri.fetch_add(1);
            if (i >= regions.size() || prog.cancel.load()) break;
            const ScanRegion& r = regions[i];
            const size_t chunk = 1 << 20;
            for (uint64_t off = 0; off < r.size; off += chunk) {
                if (prog.cancel.load()) break;
                const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, r.size - off));
                const size_t extra = pattern ? static_cast<size_t>(std::min<uint64_t>(overlap, r.size - off - n)) : 0;
                buf.resize(n + extra);
                if (!src_->read(r.addr + off, buf.data(), n + extra)) {
                    prog.bytes_done += n;
                    continue;
                }
                if (is_unknown_) {
                    std::lock_guard<std::mutex> lk(out_mu);
                    snap_->chunks.push_back({r.addr + off, buf});
                    prog.bytes_done += n;
                    continue;
                }
                std::vector<uint64_t> la, lv;
                std::vector<uint8_t> lt;
                if (pattern) {
                    const uint8_t first_m = pat_mask_[0], first_v = pat_val_[0];
                    for (size_t p = 0; p < n && p + ts <= buf.size(); p += step) {
                        if ((buf[p] & first_m) != first_v) continue;
                        if (!pat_match(buf.data() + p, pat_val_, pat_mask_)) continue;
                        la.push_back(r.addr + off + p);
                        lv.push_back(hash_bytes(buf.data() + p, ts));
                    }
                } else if (kind_ == ScanKind::AllNumeric) {
                    for (size_t p = 0; p < n; ++p) {
                        for (const TypeRef& tr : refs) {
                            const size_t s = type_size(tr.t);
                            if ((opt.fast && p % s) || p + s > n) continue;      // fast: every type at its own alignment
                            const uint64_t cur = load_bits(buf.data() + p, tr.t);
                            if (cmp_scalar(tr.t, cur, cmp, tr.a.bits, tr.b.bits)) {
                                la.push_back(r.addr + off + p);
                                lv.push_back(cur);
                                lt.push_back(static_cast<uint8_t>(tr.t));
                            }
                        }
                    }
                } else {
                    for (size_t p = 0; p + ts <= n; p += step) {
                        const uint64_t cur = load_bits(buf.data() + p, t);
                        if (cmp_scalar(t, cur, cmp, v1.bits, v2.bits)) {
                            la.push_back(r.addr + off + p);
                            lv.push_back(cur);
                        }
                    }
                }
                if (!la.empty()) {
                    std::lock_guard<std::mutex> lk(out_mu);
                    for (size_t k = 0; k < la.size(); ++k) {
                        if (addrs_.size() >= opt_.result_cap) { capped_ = true; break; }
                        addrs_.push_back(la[k]);
                        first_bits_.push_back(lv[k]);
                        last_bits_.push_back(lv[k]);
                        if (!lt.empty()) types_.push_back(lt[k]);
                    }
                    prog.results = addrs_.size();
                }
                prog.bytes_done += n;
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < nthreads; ++k) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    // the workers append in any order: sort by address (values and types move with them) so the list reads in memory order
    if (!addrs_.empty()) {
        std::vector<size_t> idx(addrs_.size());
        for (size_t k = 0; k < idx.size(); ++k) idx[k] = k;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            if (addrs_[a] != addrs_[b]) return addrs_[a] < addrs_[b];
            return !types_.empty() && types_[a] < types_[b];
        });
        std::vector<uint64_t> na(idx.size()), nf(idx.size()), nl(idx.size());
        std::vector<uint8_t> nt(types_.empty() ? 0 : idx.size());
        for (size_t k = 0; k < idx.size(); ++k) {
            na[k] = addrs_[idx[k]];
            nf[k] = first_bits_[idx[k]];
            nl[k] = last_bits_[idx[k]];
            if (!nt.empty()) nt[k] = types_[idx[k]];
        }
        addrs_ = std::move(na);
        first_bits_ = std::move(nf);
        last_bits_ = std::move(nl);
        types_ = std::move(nt);
    }
    if (is_unknown_ && snap_)
        std::sort(snap_->chunks.begin(), snap_->chunks.end(), [](const Snapshot::Chunk& a, const Snapshot::Chunk& b) { return a.addr < b.addr; });
    prog.running = false;
}

void Scanner::next_scan(NextScan cmp, const Value& v1, const Value& v2, ScanProgress& prog) {
    push_undo_state();
    const ScanType t = opt_.type;
    const bool pattern = is_pattern_kind(kind_);
    const size_t ts = pattern ? pat_len_ : type_size(t);
    unsigned step = opt_.alignment ? opt_.alignment : (opt_.fast ? static_cast<unsigned>(ts) : 1);
    if (step == 0) step = 1;

    if (is_unknown_ && snap_) {
        // the snapshot becomes the first result list: compare the snapshot to memory now
        prog.running = true;
        uint64_t total = 0;
        for (const auto& c : snap_->chunks) total += c.bytes.size();
        prog.bytes_total = total;
        prog.bytes_done = 0;
        struct Part { std::vector<uint64_t> a, f, l; std::vector<uint8_t> ty; };
        std::vector<Part> parts(snap_->chunks.size());
        std::atomic<size_t> ci{0};
        const bool all = kind_ == ScanKind::AllNumeric;
        const bool fast = opt_.fast;
        auto work = [&] {
            std::vector<uint8_t> buf;
            for (;;) {
                const size_t i = ci.fetch_add(1);
                if (i >= snap_->chunks.size() || prog.cancel.load()) break;
                const auto& c = snap_->chunks[i];
                buf.resize(c.bytes.size());
                if (!src_->read(c.addr, buf.data(), buf.size())) { prog.bytes_done += c.bytes.size(); continue; }
                Part& pt = parts[i];
                if (all) {
                    for (size_t p = 0; p < buf.size(); ++p)
                        for (ScanType at : kAllTypes) {
                            const size_t s = type_size(at);
                            if ((fast && p % s) || p + s > buf.size()) continue;
                            const uint64_t prev = load_bits(c.bytes.data() + p, at), cur = load_bits(buf.data() + p, at);
                            Value a = v1, b = v2;
                            if (at != t) { parse_value(at, v1.text(), &a); parse_value(at, v2.text(), &b); }
                            if (cmp_next(at, cur, prev, prev, cmp, a.bits, b.bits)) {
                                pt.a.push_back(c.addr + p); pt.f.push_back(prev); pt.l.push_back(cur); pt.ty.push_back(static_cast<uint8_t>(at));
                            }
                        }
                } else {
                    for (size_t p = 0; p + ts <= buf.size(); p += step) {
                        const uint64_t prev = load_bits(c.bytes.data() + p, t);
                        const uint64_t cur = load_bits(buf.data() + p, t);
                        if (cmp_next(t, cur, prev, prev, cmp, v1.bits, v2.bits)) {
                            pt.a.push_back(c.addr + p); pt.f.push_back(prev); pt.l.push_back(cur);
                        }
                    }
                }
                prog.bytes_done += c.bytes.size();
            }
        };
        const unsigned nthreads = worker_count(opt_.threads);
        std::vector<std::thread> pool;
        for (unsigned k = 1; k < nthreads; ++k) pool.emplace_back(work);
        work();
        for (auto& th : pool) th.join();
        addrs_.clear();
        first_bits_.clear();
        last_bits_.clear();
        types_.clear();
        for (Part& pt : parts) {          // the snapshot's chunks are in address order, so the merged list is too
            addrs_.insert(addrs_.end(), pt.a.begin(), pt.a.end());
            first_bits_.insert(first_bits_.end(), pt.f.begin(), pt.f.end());
            last_bits_.insert(last_bits_.end(), pt.l.begin(), pt.l.end());
            types_.insert(types_.end(), pt.ty.begin(), pt.ty.end());
        }
        is_unknown_ = false;
        snap_.reset();
        if (addrs_.size() > opt_.result_cap) {
            capped_ = true;
            addrs_.resize(opt_.result_cap);
            first_bits_.resize(opt_.result_cap);
            last_bits_.resize(opt_.result_cap);
            if (!types_.empty()) types_.resize(opt_.result_cap);
        }
        prog.results = addrs_.size();
        prog.running = false;
        return;
    }

    // narrow the existing result list
    prog.running = true;
    prog.bytes_total = addrs_.size();
    prog.bytes_done = 0;
    std::vector<uint64_t> na, nf, nl;
    std::vector<uint8_t> nt;
    na.reserve(addrs_.size());
    nf.reserve(addrs_.size());
    nl.reserve(addrs_.size());
    const bool all = !types_.empty();
    std::vector<uint8_t> buf(std::max<size_t>(ts, 8));
    for (size_t i = 0; i < addrs_.size(); ++i) {
        if (prog.cancel.load()) break;
        const ScanType rt = all ? static_cast<ScanType>(types_[i]) : t;
        const size_t rs = pattern ? ts : type_size(rt);
        if (src_->read(addrs_[i], buf.data(), rs)) {
            bool keep;
            uint64_t cur;
            if (pattern) {
                cur = hash_bytes(buf.data(), rs);
                switch (cmp) {
                    case NextScan::Exact: keep = pat_match(buf.data(), pat_val_, pat_mask_); break;
                    case NextScan::Changed: keep = cur != last_bits_[i]; break;
                    case NextScan::Unchanged: keep = cur == last_bits_[i]; break;
                    case NextScan::SameAsFirst: keep = cur == first_bits_[i]; break;
                    default: keep = true; break;           // the ordering scans mean nothing for a text or pattern
                }
            } else {
                cur = load_bits(buf.data(), rt);
                Value a = v1, b = v2;
                if (all && rt != t) {              // the reference value in this result's own type
                    parse_value(rt, v1.text(), &a);
                    parse_value(rt, v2.text(), &b);
                }
                keep = cmp_next(rt, cur, last_bits_[i], first_bits_[i], cmp, a.bits, b.bits);
            }
            if (keep) {
                na.push_back(addrs_[i]);
                nf.push_back(first_bits_[i]);
                nl.push_back(cur);
                if (all) nt.push_back(types_[i]);
            }
        }
        prog.bytes_done = i + 1;
    }
    addrs_ = std::move(na);
    first_bits_ = std::move(nf);
    last_bits_ = std::move(nl);
    types_ = std::move(nt);
    prog.results = addrs_.size();
    prog.running = false;
}

bool Scanner::read_value(uint64_t addr, ScanType t, Value* out) {
    uint8_t buf[8];
    if (!src_->read(addr, buf, type_size(t))) return false;
    out->type = t;
    out->bits = load_bits(buf, t);
    return true;
}

std::vector<ScanResult> Scanner::results(size_t start, size_t max) {
    std::vector<ScanResult> out;
    if (is_unknown_) return out;       // not materialised until the first next scan
    const bool pattern = is_pattern_kind(kind_);
    const size_t end = std::min(addrs_.size(), start + max);
    for (size_t i = start; i < end; ++i) {
        ScanResult r;
        r.addr = addrs_[i];
        r.type = types_.empty() ? opt_.type : static_cast<ScanType>(types_[i]);
        r.len = pattern ? pat_len_ : type_size(r.type);
        if (pattern) {
            // a text / pattern result carries its first 8 bytes as a u64 (the UI renders the bytes or the text itself)
            r.type = ScanType::U64;
            uint8_t b[8] = {};
            src_->read(addrs_[i], b, std::min<size_t>(8, pat_len_));
            uint64_t bits = 0;
            std::memcpy(&bits, b, 8);
            r.last = r.first = Value{ScanType::U64, bits};
            out.push_back(r);
            continue;
        }
        r.first = Value{r.type, first_bits_[i]};
        Value cur;
        if (read_value(addrs_[i], r.type, &cur)) r.last = cur;
        else r.last = Value{r.type, last_bits_[i]};
        out.push_back(r);
    }
    return out;
}

}  // namespace nxrt::cheats
