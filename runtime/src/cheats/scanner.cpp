// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Memory scanner (scanner.h).
#include "scanner.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "aob.h"
#include "gmem.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

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
#if defined(__APPLE__)
    // mincore: a page of anonymous memory that is neither resident nor paged out (compressed or swapped: MINCORE_PAGED_OUT) was never
    // touched and reads as zeros. Measured on the Mac (scratch/pokopia-cheats/tools/fp_probe.c, a named memory entry as the heap is mapped):
    // an untouched page answers 0x80 (anonymous only), a written one 0x87, and reading 768 MiB of untouched pages raised the footprint
    // from 257 MB to 1026 MB. A page of a file mapping is always read.
    bool untouched(uint64_t addr, size_t n, size_t* page, std::vector<uint8_t>* flags) override {
        const uint64_t pg = static_cast<uint64_t>(getpagesize());
        if (pg == 0 || n == 0) return false;
        const uint64_t lo = addr & ~(pg - 1), hi = (addr + n + pg - 1) & ~(pg - 1);
        flags->assign(static_cast<size_t>((hi - lo) / pg), 0);
        if (mincore(reinterpret_cast<void*>(lo), static_cast<size_t>(hi - lo), reinterpret_cast<char*>(flags->data())) != 0) return false;
        constexpr uint8_t kIncore = 0x01, kPagedOut = 0x20, kAnonymous = 0x80;   // MINCORE_INCORE, MINCORE_PAGED_OUT, MINCORE_ANONYMOUS
        for (uint8_t& v : *flags) v = (v & kAnonymous) && !(v & (kIncore | kPagedOut)) ? 1 : 0;
        *page = static_cast<size_t>(pg);
        return true;
    }
#endif
};

}  // namespace

std::shared_ptr<MemSource> live_source() { return std::make_shared<GmemSource>(); }

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

// ---- the unknown value's snapshot ---------------------------------------------------------------------------------------------------------

namespace {
constexpr size_t kChunk = size_t{1} << 20;   // the unit the scanner reads, keeps in a snapshot, and splits the work by
constexpr size_t kBlock = 4096;              // a snapshot keeps an all-zero block of this size as one bit
constexpr uint64_t kNoFile = ~uint64_t{0};

uint64_t host_ram() {
#if defined(_WIN32)
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) return m.ullTotalPhys;
#elif defined(__APPLE__)
    uint64_t v = 0;
    size_t n = sizeof v;
    if (sysctlbyname("hw.memsize", &v, &n, nullptr, 0) == 0 && v) return v;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && ps > 0) return static_cast<uint64_t>(pages) * static_cast<uint64_t>(ps);
#endif
    return uint64_t{8} << 30;
}
// RAM for the snapshot: a sixteenth of the host's (2 GiB of a 32 GiB machine and up, 1 GiB of a 16 GiB one), at least 128 MiB.
uint64_t snapshot_budget() { return std::clamp<uint64_t>(host_ram() / 16, uint64_t{128} << 20, uint64_t{2} << 30); }

bool zero_block(const uint8_t* p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w) return false;
    }
    for (; i < n; ++i)
        if (p[i]) return false;
    return true;
}

int seek_to(std::FILE* f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(off), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(off), SEEK_SET);
#endif
}

unsigned process_id() {
#if defined(_WIN32)
    return static_cast<unsigned>(_getpid());
#else
    return static_cast<unsigned>(getpid());
#endif
}
}  // namespace

// The bytes of the snapshots: per 1 MiB chunk a blob whose all-zero 4 KiB blocks are only bits; the rest in RAM while the store's RAM is
// under its budget, else in the spill file (first fit over the space blobs gave back). Thread safe.
class SnapStore : public std::enable_shared_from_this<SnapStore> {
public:
    struct Blob {
        std::shared_ptr<SnapStore> store;
        uint32_t size = 0;
        std::vector<uint64_t> zero;          // a bit per block: all zero, not kept
        std::vector<uint8_t> ram;            // the other blocks, packed, when kept in RAM
        uint64_t file_off = kNoFile;         // or where they are in the spill file
        uint64_t packed = 0;                 // their bytes
        ~Blob() {
            if (store) store->release(*this);
        }
    };
    using BlobRef = std::shared_ptr<const Blob>;

    SnapStore(uint64_t budget, std::string dir) : budget_(budget), dir_(std::move(dir)) {}
    ~SnapStore() { close_file(); }
    uint64_t budget() const { return budget_; }
    const std::string& dir() const { return dir_; }

    // Keeps n bytes; null when the spill file could not take them (error()).
    BlobRef put(const uint8_t* p, size_t n) {
        thread_local std::vector<uint8_t> packed;
        packed.clear();
        auto b = std::make_shared<Blob>();
        b->size = static_cast<uint32_t>(n);
        const size_t nb = (n + kBlock - 1) / kBlock;
        b->zero.assign((nb + 63) / 64, 0);
        for (size_t k = 0; k < nb; ++k) {
            const size_t o = k * kBlock, len = std::min(kBlock, n - o);
            if (zero_block(p + o, len)) b->zero[k >> 6] |= uint64_t{1} << (k & 63);
            else packed.insert(packed.end(), p + o, p + o + len);
        }
        b->packed = packed.size();
        std::lock_guard<std::mutex> lk(mu_);
        if (!packed.empty()) {
            if (ram_ + packed.size() <= budget_) {
                b->ram.assign(packed.begin(), packed.end());
                ram_ += packed.size();
                ram_peak_ = std::max(ram_peak_, ram_);
            } else {
                if (!f_ && !open_file()) return nullptr;
                const uint64_t off = alloc(packed.size());
                if (seek_to(f_, off) != 0 || std::fwrite(packed.data(), 1, packed.size(), f_) != packed.size()) {
                    error_ = "could not write the search's snapshot file " + path_ + " (is the disk full?)";
                    free_extent(off, packed.size());
                    return nullptr;
                }
                b->file_off = off;
                file_live_ += packed.size();
                file_peak_ = std::max(file_peak_, end_);
            }
        }
        bytes_ += n;
        zero_ += n - b->packed;
        b->store = shared_from_this();
        return b;
    }
    // The blob's bytes into dst (b.size bytes); false when the spill file could not be read back.
    bool get(const Blob& b, uint8_t* dst) const {
        thread_local std::vector<uint8_t> tmp;
        const uint8_t* src = b.ram.data();
        if (b.file_off != kNoFile) {
            tmp.resize(static_cast<size_t>(b.packed));
            std::lock_guard<std::mutex> lk(mu_);
            if (!f_ || seek_to(f_, b.file_off) != 0 || std::fread(tmp.data(), 1, tmp.size(), f_) != tmp.size()) {
                error_ = "could not read back the search's snapshot file " + path_;
                return false;
            }
            src = tmp.data();
        }
        size_t q = 0;
        for (size_t k = 0, nb = (b.size + kBlock - 1) / kBlock; k < nb; ++k) {
            const size_t o = k * kBlock, len = std::min<size_t>(kBlock, b.size - o);
            if ((b.zero[k >> 6] >> (k & 63)) & 1) {
                std::memset(dst + o, 0, len);
            } else {
                std::memcpy(dst + o, src + q, len);
                q += len;
            }
        }
        return true;
    }
    SnapshotStats stats() const {
        std::lock_guard<std::mutex> lk(mu_);
        SnapshotStats st;
        st.bytes = bytes_;
        st.zero = zero_;
        st.ram = ram_;
        st.ram_peak = ram_peak_;
        st.ram_budget = budget_;
        st.file = file_live_;
        st.file_peak = file_peak_;
        st.file_path = path_;
        return st;
    }
    std::string error() const {
        std::lock_guard<std::mutex> lk(mu_);
        return error_;
    }

private:
    void release(const Blob& b) {
        std::lock_guard<std::mutex> lk(mu_);
        bytes_ -= b.size;
        zero_ -= b.size - b.packed;
        ram_ -= b.ram.size();
        if (b.file_off != kNoFile) {
            file_live_ -= b.packed;
            free_extent(b.file_off, b.packed);
            if (file_live_ == 0) close_file();
        }
    }
    // Opens the spill file in the store's folder (else the temp folder: a game's cheats folder may not be there yet) and takes it out of
    // the folder at once: POSIX unlinks the open file, Windows' "D" deletes it when it is closed (by the system too, however the process
    // ends). The space goes back when the last blob in it goes.
    bool open_file() {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<fs::path> dirs;
        if (!dir_.empty()) dirs.push_back(fs::path(reinterpret_cast<const char8_t*>(dir_.c_str())));
        if (const fs::path tmp = fs::temp_directory_path(ec); !ec && !tmp.empty()) dirs.push_back(tmp);
        static std::atomic<unsigned> seq{0};
        std::string tried;
        for (const fs::path& dir : dirs) {
            char name[80];
            std::snprintf(name, sizeof name, "nxrt-search-%u-%u.snapshot", process_id(), seq.fetch_add(1));
            const fs::path p = dir / name;
            const auto u8 = p.u8string();
            path_.assign(u8.begin(), u8.end());
#if defined(_WIN32)
            f_ = _wfopen(p.c_str(), L"w+bD");
#else
            f_ = std::fopen(p.c_str(), "w+b");
            if (f_) ::unlink(p.c_str());
#endif
            if (f_) return true;
            tried += (tried.empty() ? "" : ", ") + path_;
        }
        error_ = "could not create the search's snapshot file (" + (tried.empty() ? std::string("no folder") : tried) + ")";
        return false;
    }
    void close_file() {
        if (f_) std::fclose(f_);
        f_ = nullptr;
        end_ = 0;
        free_.clear();
    }
    uint64_t alloc(uint64_t n) {
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second < n) continue;
            const uint64_t off = it->first, rest = it->second - n;
            free_.erase(it);
            if (rest) free_[off + n] = rest;
            return off;
        }
        const uint64_t off = end_;
        end_ += n;
        return off;
    }
    void free_extent(uint64_t off, uint64_t n) {
        if (n == 0) return;
        auto it = free_.emplace(off, n).first;
        if (auto nx = std::next(it); nx != free_.end() && it->first + it->second == nx->first) {
            it->second += nx->second;
            free_.erase(nx);
        }
        if (it != free_.begin()) {
            auto pv = std::prev(it);
            if (pv->first + pv->second == it->first) {
                pv->second += it->second;
                free_.erase(it);
                it = pv;
            }
        }
        if (it->first + it->second == end_) {
            end_ = it->first;
            free_.erase(it);
        }
    }

    const uint64_t budget_;
    const std::string dir_;
    mutable std::mutex mu_;
    std::FILE* f_ = nullptr;
    std::string path_;
    mutable std::string error_;
    uint64_t end_ = 0, file_live_ = 0, file_peak_ = 0, ram_ = 0, ram_peak_ = 0, bytes_ = 0, zero_ = 0;
    std::map<uint64_t, uint64_t> free_;   // spill file space blobs gave back: offset -> bytes
};

// An unknown value's state: per chunk the bytes at the first scan, the bytes at the last scan (only the candidates' bytes: the rest zeroed,
// so the blocks without a candidate cost a bit), and the candidates as a bit per slot (null: every slot still is one).
struct Scanner::Snapshot {
    struct Chunk {
        uint64_t addr = 0;
        uint32_t size = 0;
        SnapStore::BlobRef first, last, cand;
        uint64_t count = 0;           // candidates in the chunk
    };
    std::vector<Chunk> chunks;        // in address order
    uint64_t count = 0;               // candidates in all
};

namespace {
// The slots of one chunk: per type (one, or the six of an all-numeric search) the offsets 0, step, 2*step, ... whose value fits in the chunk,
// as the first scan of a known value walks them. A slot's candidate bit is its plane's base + offset / step.
struct Plane {
    ScanType t;
    size_t size, step, base, n;
};
std::vector<Plane> planes_for(size_t chunk, bool all, ScanType t, unsigned step, bool fast) {
    std::vector<Plane> out;
    size_t base = 0;
    auto add = [&](ScanType at, size_t st) {
        const size_t s = type_size(at);
        const size_t n = chunk >= s ? (chunk - s) / st + 1 : 0;
        out.push_back({at, s, st, base, n});
        base += n;
    };
    if (all) {
        for (ScanType at : kAllTypes) add(at, fast ? type_size(at) : 1);
    } else {
        add(t, step ? step : 1);
    }
    return out;
}
size_t slot_count(const std::vector<Plane>& pl) { return pl.empty() ? 0 : pl.back().base + pl.back().n; }
bool bit_at(const uint64_t* bits, size_t b) { return !bits || ((bits[b >> 6] >> (b & 63)) & 1); }

// fn(plane, offset, bit) for every candidate (every slot when bits is null), plane by plane.
template <class Fn>
void each_slot(const std::vector<Plane>& planes, const uint64_t* bits, Fn&& fn) {
    for (const Plane& pl : planes) {
        for (size_t k = 0; k < pl.n;) {
            const size_t b = pl.base + k;
            if (bits) {
                const uint64_t w = bits[b >> 6] >> (b & 63);
                if (!w) { k += 64 - (b & 63); continue; }
                if (!(w & 1)) { k += static_cast<size_t>(std::countr_zero(w)); continue; }
            }
            fn(pl, k * pl.step, b);
            ++k;
        }
    }
}
// The same in address order (an all-numeric search: each offset's types in kAllTypes order, as a first scan lists them); fn returns false
// to stop.
template <class Fn>
bool each_slot_ordered(const std::vector<Plane>& planes, const uint64_t* bits, size_t chunk, Fn&& fn) {
    if (planes.size() == 1) {
        bool go = true;
        const Plane& pl = planes[0];
        for (size_t k = 0; k < pl.n && go;) {
            const size_t b = pl.base + k;
            if (bits) {
                const uint64_t w = bits[b >> 6] >> (b & 63);
                if (!w) { k += 64 - (b & 63); continue; }
                if (!(w & 1)) { k += static_cast<size_t>(std::countr_zero(w)); continue; }
            }
            go = fn(pl, k * pl.step);
            ++k;
        }
        return go;
    }
    for (size_t p = 0; p < chunk; ++p)
        for (const Plane& pl : planes) {
            if (p % pl.step) continue;
            const size_t k = p / pl.step;
            if (k < pl.n && bit_at(bits, pl.base + k) && !fn(pl, p)) return false;
        }
    return true;
}

bool blob_get(const SnapStore::Blob& b, uint8_t* dst) {
    if (b.store) return b.store->get(b, dst);
    std::memset(dst, 0, b.size);
    return true;
}
bool bits_get(const SnapStore::Blob& b, std::vector<uint64_t>* out) {
    out->assign((b.size + 7) / 8, 0);
    return blob_get(b, reinterpret_cast<uint8_t*>(out->data()));
}

// Reads [addr, addr+n): the pages the source says the game never touched are zeros, not read (MemSource::untouched).
bool read_chunk(MemSource& src, uint64_t addr, uint8_t* dst, size_t n, std::vector<uint8_t>& flags) {
    size_t page = 0;
    if (n == 0 || !src.untouched(addr, n, &page, &flags) || page == 0) return src.read(addr, dst, n);
    const uint64_t base = addr & ~(static_cast<uint64_t>(page) - 1), end = addr + n;
    auto zero_at = [&](uint64_t a) {
        const size_t i = static_cast<size_t>((a - base) / page);
        return i < flags.size() && flags[i] != 0;
    };
    for (uint64_t a = addr; a < end;) {
        const bool z = zero_at(a);
        uint64_t e = std::min(end, base + ((a - base) / page + 1) * page);
        while (e < end && zero_at(e) == z) e = std::min(end, e + page);
        if (z) std::memset(dst + (a - addr), 0, static_cast<size_t>(e - a));
        else if (!src.read(a, dst + (a - addr), static_cast<size_t>(e - a))) return false;
        a = e;
    }
    return true;
}
}  // namespace

Scanner::Scanner(std::shared_ptr<MemSource> src) : src_(std::move(src)) {}

size_t Scanner::result_count() const {
    if (is_unknown_) return snap_ ? static_cast<size_t>(snap_->count) : 0;
    return addrs_.size();
}

void Scanner::clear() {
    addrs_.clear();
    first_bits_.clear();
    last_bits_.clear();
    types_.clear();
    snap_.reset();
    undo_.clear();                      // the undo steps may hold a snapshot: its RAM and spill file go too
    is_unknown_ = false;
    capped_ = false;
    error_.clear();
}

SnapshotStats Scanner::snapshot_stats() const { return store_ ? store_->stats() : SnapshotStats{}; }

unsigned Scanner::slot_step() const {
    const size_t ts = type_size(opt_.type);
    const unsigned step = opt_.alignment ? opt_.alignment : (opt_.fast ? static_cast<unsigned>(ts) : 1u);
    return step ? step : 1u;
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
    // a snapshot stands for the scanned memory (RAM up to the budget, the rest in the spill file): the steps keep the newest one only, and
    // the steps before it go with it (undo never skips a step)
    for (size_t k = undo_.size(); k-- > 1;)
        if (undo_[k].snap) {
            undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(k));
            break;
        }
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
    error_.clear();
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
    if (is_unknown_) {
        // the snapshot: the first next scan compares memory to it (no list of every address: 11 GB of heap is 2.8 billion u32 slots)
        const uint64_t budget = opt.snapshot_ram ? opt.snapshot_ram : snapshot_budget();
        if (!store_ || store_->budget() != budget || store_->dir() != opt.spill_dir) store_ = std::make_shared<SnapStore>(budget, opt.spill_dir);
        snap_ = std::make_shared<Snapshot>();
    }
    const bool all = kind_ == ScanKind::AllNumeric;

    // all numeric types: the reference text parsed as each type (a type it does not parse as is skipped), so "100" also finds 100.0f
    struct TypeRef { ScanType t; Value a, b; };
    std::vector<TypeRef> refs;
    if (all) {
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
    std::atomic<bool> failed{false};
    const unsigned nthreads = worker_count(opt.threads);
    const size_t overlap = ts > 1 ? ts - 1 : 0;   // a text or pattern may straddle two chunks
    auto work = [&] {
        std::vector<uint8_t> buf, flags;
        for (;;) {
            const size_t i = ri.fetch_add(1);
            if (i >= regions.size() || prog.cancel.load() || failed.load()) break;
            const ScanRegion& r = regions[i];
            for (uint64_t off = 0; off < r.size; off += kChunk) {
                if (prog.cancel.load() || failed.load()) break;
                const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, r.size - off));
                const size_t extra = pattern ? static_cast<size_t>(std::min<uint64_t>(overlap, r.size - off - n)) : 0;
                buf.resize(n + extra);
                if (!read_chunk(*src_, r.addr + off, buf.data(), n + extra, flags)) {
                    prog.bytes_done += n;
                    continue;
                }
                if (is_unknown_) {
                    SnapStore::BlobRef b = store_->put(buf.data(), n);
                    if (!b) { failed = true; break; }
                    const uint64_t slots = slot_count(planes_for(n, all, t, step, opt.fast));
                    std::lock_guard<std::mutex> lk(out_mu);
                    snap_->chunks.push_back({r.addr + off, static_cast<uint32_t>(n), b, b, nullptr, slots});
                    snap_->count += slots;
                    prog.results = snap_->count;
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
                } else if (all) {
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
    if (failed.load()) {                // the spill file could not take the snapshot: no half snapshot
        error_ = store_->error();
        snap_.reset();
        is_unknown_ = false;
        prog.results = 0;
        prog.running = false;
        return;
    }
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

// A next scan of an unknown value: each chunk's candidates are compared with memory now; the survivors' bytes become the chunk's last
// values (the others zeroed: their blocks then cost a bit) and their bits the new candidates. Once few enough are left (result_cap), they
// become the result list. Peak memory: the store's budget, plus a few chunks per worker.
void Scanner::next_snapshot(NextScan cmp, const Value& v1, const Value& v2, ScanProgress& prog) {
    const Snapshot& old = *snap_;
    const ScanType t = opt_.type;
    const bool all = kind_ == ScanKind::AllNumeric;
    const unsigned step = slot_step();
    const bool fast = opt_.fast;
    const bool need_first = cmp == NextScan::SameAsFirst;
    // the reference values in each type (an all-numeric search: the text as each type, as the list's next scan reads them)
    uint64_t ref_a[6], ref_b[6];
    for (ScanType at : kAllTypes) {
        Value a = v1, b = v2;
        if (all && at != t) {
            parse_value(at, v1.text(), &a);
            parse_value(at, v2.text(), &b);
        }
        ref_a[static_cast<int>(at)] = a.bits;
        ref_b[static_cast<int>(at)] = b.bits;
    }
    if (!store_) store_ = std::make_shared<SnapStore>(snapshot_budget(), std::string());
    auto next = std::make_shared<Snapshot>();
    next->chunks.resize(old.chunks.size());
    uint64_t total_bytes = 0;
    for (const auto& c : old.chunks) total_bytes += c.size;
    prog.running = true;
    prog.bytes_total = total_bytes;
    prog.bytes_done = 0;
    prog.results = 0;
    std::atomic<size_t> ci{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> failed{false};
    auto work = [&] {
        std::vector<uint8_t> cur, last, first, keep, flags;
        std::vector<uint64_t> cand, bits;
        for (;;) {
            const size_t i = ci.fetch_add(1);
            if (i >= old.chunks.size() || prog.cancel.load() || failed.load()) break;
            const Snapshot::Chunk& c = old.chunks[i];
            Snapshot::Chunk& o = next->chunks[i];
            o.addr = c.addr;
            o.size = c.size;
            cur.resize(c.size);
            if (!read_chunk(*src_, c.addr, cur.data(), c.size, flags)) {   // unreadable now: its candidates go
                prog.bytes_done += c.size;
                continue;
            }
            const bool same = c.first == c.last;      // the first next scan: the first values are the last ones
            if (!need_first && c.last->packed == 0 && zero_block(cur.data(), c.size)) {
                // all zero at the last scan and now (most of a heap the game never used): each type's candidates all answer the same
                int keep_n = 0, planes_n = 0;
                for (const Plane& pl : planes_for(c.size, all, t, step, fast)) {
                    ++planes_n;
                    const int ti = static_cast<int>(pl.t);
                    keep_n += cmp_next(pl.t, 0, 0, 0, cmp, ref_a[ti], ref_b[ti]) ? 1 : 0;
                }
                if (keep_n == 0 || keep_n == planes_n) {
                    if (keep_n) {
                        o.first = c.first;
                        o.last = c.last;
                        o.cand = c.cand;
                        o.count = c.count;
                        total += c.count;
                    }
                    prog.bytes_done += c.size;
                    prog.results = total.load();
                    continue;
                }
            }
            last.resize(c.size);
            bool ok = blob_get(*c.last, last.data());
            if (ok && need_first && !same) {
                first.resize(c.size);
                ok = blob_get(*c.first, first.data());
            }
            if (ok && c.cand) ok = bits_get(*c.cand, &cand);
            if (!ok) { failed = true; break; }
            const uint8_t* fp = need_first && !same ? first.data() : last.data();
            const auto planes = planes_for(c.size, all, t, step, fast);
            const size_t slots = slot_count(planes);
            bits.assign((slots + 63) / 64, 0);
            keep.assign(c.size, 0);
            uint64_t n = 0;
            each_slot(planes, c.cand ? cand.data() : nullptr, [&](const Plane& pl, size_t p, size_t b) {
                const uint64_t prev = load_bits(last.data() + p, pl.t), now = load_bits(cur.data() + p, pl.t);
                const uint64_t f = need_first ? load_bits(fp + p, pl.t) : prev;
                const int ti = static_cast<int>(pl.t);
                if (!cmp_next(pl.t, now, prev, f, cmp, ref_a[ti], ref_b[ti])) return;
                bits[b >> 6] |= uint64_t{1} << (b & 63);
                std::memcpy(keep.data() + p, cur.data() + p, pl.size);
                ++n;
            });
            if (n) {
                o.count = n;
                o.last = store_->put(keep.data(), c.size);
                o.cand = n == slots ? nullptr : store_->put(reinterpret_cast<const uint8_t*>(bits.data()), bits.size() * 8);
                o.first = c.first;
                if (n * 2 <= c.count) {
                    // half the candidates or more went: keep only the survivors' first-scan bytes (the full copy goes with the undo step)
                    if (!need_first || same) {
                        if (same) first.assign(last.begin(), last.end());
                        else { first.resize(c.size); ok = blob_get(*c.first, first.data()); }
                    }
                    if (ok) {
                        keep.assign(c.size, 0);
                        each_slot(planes, bits.data(), [&](const Plane& pl, size_t p, size_t) { std::memcpy(keep.data() + p, first.data() + p, pl.size); });
                        o.first = store_->put(keep.data(), c.size);
                    }
                }
                if (!ok || !o.last || !o.first || (n != slots && !o.cand)) { failed = true; break; }
                total += n;
            }
            prog.bytes_done += c.size;
            prog.results = total.load();
        }
    };
    const unsigned nthreads = worker_count(opt_.threads);
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < nthreads; ++k) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    if (failed.load() || prog.cancel.load()) {
        if (failed.load()) error_ = store_->error();
        undo();                         // stopped or failed: the candidates stay as they were before this scan
        prog.results = result_count();
        prog.running = false;
        return;
    }
    next->chunks.erase(std::remove_if(next->chunks.begin(), next->chunks.end(), [](const Snapshot::Chunk& c) { return c.count == 0; }),
                       next->chunks.end());
    next->count = total.load();
    if (next->count <= opt_.result_cap) {
        if (!lists_from(*next)) {
            error_ = store_->error();
            undo();
            prog.results = result_count();
            prog.running = false;
            return;
        }
        is_unknown_ = false;
        snap_.reset();
    } else {
        snap_ = std::move(next);
    }
    prog.results = result_count();
    prog.running = false;
}

// The candidates of a snapshot as the result list, in address order.
bool Scanner::lists_from(const Snapshot& s) {
    const bool all = kind_ == ScanKind::AllNumeric;
    const unsigned step = slot_step();
    addrs_.clear();
    first_bits_.clear();
    last_bits_.clear();
    types_.clear();
    addrs_.reserve(static_cast<size_t>(s.count));
    first_bits_.reserve(static_cast<size_t>(s.count));
    last_bits_.reserve(static_cast<size_t>(s.count));
    if (all) types_.reserve(static_cast<size_t>(s.count));
    std::vector<uint8_t> first, last;
    std::vector<uint64_t> cand;
    for (const auto& c : s.chunks) {
        first.resize(c.size);
        last.resize(c.size);
        if (!blob_get(*c.first, first.data()) || !blob_get(*c.last, last.data())) return false;
        if (c.cand && !bits_get(*c.cand, &cand)) return false;
        each_slot_ordered(planes_for(c.size, all, opt_.type, step, opt_.fast), c.cand ? cand.data() : nullptr, c.size, [&](const Plane& pl, size_t p) {
            addrs_.push_back(c.addr + p);
            first_bits_.push_back(load_bits(first.data() + p, pl.t));
            last_bits_.push_back(load_bits(last.data() + p, pl.t));
            if (all) types_.push_back(static_cast<uint8_t>(pl.t));
            return true;
        });
    }
    capped_ = false;
    return true;
}

void Scanner::next_scan(NextScan cmp, const Value& v1, const Value& v2, ScanProgress& prog) {
    push_undo_state();
    error_.clear();
    const ScanType t = opt_.type;
    const bool pattern = is_pattern_kind(kind_);
    const size_t ts = pattern ? pat_len_ : type_size(t);

    if (is_unknown_ && snap_) {
        next_snapshot(cmp, v1, v2, prog);
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
    if (is_unknown_) {
        // the candidates of a snapshot, as many as asked from `start` in address order: the value at the first scan and the one now
        if (!snap_ || max == 0) return out;
        const bool all = kind_ == ScanKind::AllNumeric;
        const unsigned step = slot_step();
        uint64_t skip = start;
        std::vector<uint8_t> first;
        std::vector<uint64_t> cand;
        for (const auto& c : snap_->chunks) {
            if (out.size() >= max) break;
            if (skip >= c.count) { skip -= c.count; continue; }
            first.resize(c.size);
            if (!blob_get(*c.first, first.data())) break;
            if (c.cand && !bits_get(*c.cand, &cand)) break;
            each_slot_ordered(planes_for(c.size, all, opt_.type, step, opt_.fast), c.cand ? cand.data() : nullptr, c.size, [&](const Plane& pl, size_t p) {
                if (skip) { --skip; return true; }
                ScanResult r;
                r.addr = c.addr + p;
                r.type = pl.t;
                r.len = pl.size;
                r.first = Value{pl.t, load_bits(first.data() + p, pl.t)};
                Value now;
                r.last = read_value(r.addr, pl.t, &now) ? now : r.first;
                out.push_back(r);
                return out.size() < max;
            });
        }
        return out;
    }
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
