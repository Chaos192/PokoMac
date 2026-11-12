// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Memory scanner for the cheats overlay: a Cheat Engine-class value search over guest memory. Multithreaded and chunked so it stays
// responsive on a 12+ GiB Z-A heap; the game keeps running unless the caller asks for a pause.
//
//   - first scan: exact value, unknown initial value, value in a range, greater / less / >= / <=;
//   - next scan: exact, changed, unchanged, increased, decreased, increased by N, decreased by N, in a range, greater / less, same as the
//     first scan (every result keeps its first-scan value);
//   - numeric types u8/u16/u32/u64, float, double, and "all numeric types" (every type at every aligned offset);
//   - string (UTF-8 and UTF-16LE, optionally ignoring ASCII case) and array-of-bytes with `??` / nibble wildcards (ScanKind; aob.h);
//   - alignment, fast scan, a region filter (main, heap, alias, stack, writable only, a custom range), a result cap, undo of one scan.
//
// The scanner reads memory through a MemSource so it can be tested without the runtime. The live source (LiveSource) is gmem.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nxrt::cheats {

enum class ScanType : uint8_t { U8, U16, U32, U64, F32, F64 };
size_t type_size(ScanType t);
const char* type_name(ScanType t);
bool type_is_float(ScanType t);

// How a first scan matches.
enum class FirstScan : uint8_t { Exact, Unknown, Between, Greater, Less, GreaterOrEqual, LessOrEqual };
// How a next scan narrows the results.
enum class NextScan : uint8_t { Exact, Changed, Unchanged, Increased, Decreased, IncreasedBy, DecreasedBy, Between, Greater, Less, SameAsFirst, Unknown };

// A scalar value of a scan type, stored in 8 bytes. Comparisons use the type.
struct Value {
    ScanType type = ScanType::U32;
    uint64_t bits = 0;      // the value's bytes, little-endian in the low bytes
    static Value u(ScanType t, uint64_t v) { return {t, v}; }
    double as_double() const;
    int64_t as_int() const;
    void set_double(double d);
    std::string text() const;         // for display and export
};
// Parse a value from text for a type ("1000", "0x3E8", "3.5"); false on a bad string.
bool parse_value(ScanType t, const std::string& s, Value* out);

// Which guest regions a scan covers.
struct RegionFilter {
    bool main = true, heap = true, alias = true, stack = false, code = false, other = false;
    bool writable_only = false;       // only pages the guest can write
    uint64_t lo = 0, hi = 0;          // a custom range; 0,0 = no custom clamp
};

// A memory region the scanner walks.
struct ScanRegion {
    uint64_t addr = 0, size = 0;
    uint32_t perm = 0;                // 1 R, 2 W, 4 X
    const char* kind = "";
};

// Where the scanner reads memory. The live one is gmem (LiveSource); tests pass their own.
class MemSource {
public:
    virtual ~MemSource() = default;
    virtual std::vector<ScanRegion> regions(const RegionFilter& f) = 0;
    // Copies [addr, addr+n) into dst; false (and the scanner skips the chunk) if any of it is unreadable.
    virtual bool read(uint64_t addr, void* dst, size_t n) = 0;
};

// What a scan looks for: one numeric type, every numeric type at once, a text, or a byte pattern.
enum class ScanKind : uint8_t { Numeric, AllNumeric, Utf8, Utf16, Aob };
const char* kind_name(ScanKind k);

struct ScanOptions {
    ScanType type = ScanType::U32;
    ScanKind kind = ScanKind::Numeric;
    bool all_numeric = false;         // same as kind = AllNumeric (kept for callers that set it): every numeric type, each at its own alignment
    std::string text;                 // Utf8 / Utf16: the text; Aob: the pattern ("48 8B ?? 4?")
    bool ignore_case = false;         // Utf8 / Utf16: ASCII letters match either case
    unsigned alignment = 0;           // 0 = the type's own size; 1 = every byte; else the given step
    bool fast = true;                 // align to `alignment`; false scans every byte
    RegionFilter filter;
    size_t result_cap = 2000000;      // stop collecting results past this many (the count is still reported)
    unsigned threads = 0;             // 0 = hardware concurrency, capped
};

struct ScanProgress {
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint64_t> results{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> running{false};
};

// One result: the guest address, the value at the last scan, and the value at the first scan (for "same as the first scan").
struct ScanResult {
    uint64_t addr = 0;
    Value last;
    Value first;
    ScanType type = ScanType::U32;    // the result's own type (an all-numeric scan mixes them)
    size_t len = 4;                   // bytes the match covers (a text or pattern match: its length)
};

class Scanner {
public:
    explicit Scanner(std::shared_ptr<MemSource> src);

    // A first scan. `cmp`: Exact/Between/Greater/... use v1 (and v2 for Between). Unknown keeps a snapshot for the next scan. Runs on the
    // calling thread but spreads the work across worker threads; progress updates `prog` live and honours prog.cancel.
    void first_scan(const ScanOptions& opt, FirstScan cmp, const Value& v1, const Value& v2, ScanProgress& prog);
    // A next scan over the current results. For Unknown first scans, the first next scan turns the snapshot into results.
    void next_scan(NextScan cmp, const Value& v1, const Value& v2, ScanProgress& prog);

    bool can_undo() const { return !undo_.empty(); }
    void undo();                      // restore the result set before the last scan
    void clear();                     // drop results and the snapshot

    size_t result_count() const;      // total matches (may exceed the results actually stored)
    bool capped() const { return capped_; }
    ScanType type() const { return opt_.type; }
    ScanKind kind() const { return kind_; }
    size_t match_len() const { return pat_len_; }     // text / pattern scans: bytes per match
    // Up to `max` results from `start`, current values refreshed from memory.
    std::vector<ScanResult> results(size_t start, size_t max);
    // The value at `addr` of the scan's type, read now.
    bool read_value(uint64_t addr, ScanType t, Value* out);

private:
    struct Snapshot;
    std::shared_ptr<MemSource> src_;
    ScanOptions opt_;
    bool is_unknown_ = false;          // the current results come from an Unknown first scan (snapshot, not a result list)
    bool capped_ = false;
    std::vector<uint64_t> addrs_;      // result addresses
    std::vector<uint64_t> first_bits_; // first-scan value bits, parallel to addrs_
    std::vector<uint64_t> last_bits_;  // last-scan value bits, parallel to addrs_ (text / pattern: a hash of the matched bytes)
    std::vector<uint8_t> types_;       // AllNumeric: each result's ScanType, parallel to addrs_
    ScanKind kind_ = ScanKind::Numeric;
    std::vector<uint8_t> pat_val_, pat_mask_;   // Utf8 / Utf16 / Aob: the pattern the results matched
    size_t pat_len_ = 0;
    std::shared_ptr<Snapshot> snap_;   // Unknown first scan: the scanned bytes
    // The result set before each of the last few scans (undo).
    struct UndoState {
        std::vector<uint64_t> addrs, first, last;
        std::vector<uint8_t> types, pat_val, pat_mask;
        std::shared_ptr<Snapshot> snap;
        bool unknown = false;
        ScanKind kind = ScanKind::Numeric;
        ScanType type = ScanType::U32;
        size_t pat_len = 0;
    };
    std::vector<UndoState> undo_;
    void push_undo_state();
};

// The live memory source over gmem.
std::shared_ptr<MemSource> live_source();

}  // namespace nxrt::cheats
