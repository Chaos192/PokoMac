// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin: errors, lock slow paths, WaitOnAddress, time, strings, environment, modules, processor information, locale, UI.
#include <windows.h>

#include "nxwin_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

extern char** environ;

namespace nxwin {

namespace {
thread_local DWORD t_last_error = 0;
}

DWORD errno_to_win32(int e) {
    switch (e) {
        case 0: return ERROR_SUCCESS;
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_ALREADY_EXISTS;
        case ENOSPC: case EDQUOT: return ERROR_DISK_FULL;
        case EBADF: return ERROR_INVALID_HANDLE;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        case EINVAL: return ERROR_INVALID_PARAMETER;
        case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
        case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
        case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
        case EBUSY: return ERROR_BUSY;
        case EXDEV: return ERROR_NOT_SAME_DEVICE;
        case EISDIR: return ERROR_DIRECTORY;
        case ETIMEDOUT: return ERROR_TIMEOUT;
        case ENOSYS: case ENOTSUP: return ERROR_NOT_SUPPORTED;
        case EPIPE: return ERROR_BROKEN_PIPE;
        case EIO: return ERROR_CRC;
        case EFAULT: return ERROR_NOACCESS;
        default: return 0x20000000u | static_cast<DWORD>(e);   // customer bit: unmapped errno
    }
}

// ---- SRWLOCK / CONDITION_VARIABLE / WaitOnAddress -----------------------------------------------------------------------------------------

uint64_t mono_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

bool wait_on_address(void* addr, uint64_t value, size_t size, DWORD timeout_ms) {
    for (;;) {
        int r;
        if (timeout_ms == INFINITE) {
            r = os_sync_wait_on_address(addr, value, size, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        } else {
            r = os_sync_wait_on_address_with_timeout(addr, value, size, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                     static_cast<uint64_t>(timeout_ms) * 1000000ull);
        }
        if (r >= 0) return true;
        if (errno == ETIMEDOUT) return false;
        if (errno == EINTR || errno == EFAULT) return true;   // spurious: the caller re-checks its condition
        return true;
    }
}

void wake_address(void* addr, size_t size, bool all) {
    if (all) os_sync_wake_by_address_all(addr, size, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    else os_sync_wake_by_address_any(addr, size, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

namespace {
inline void cpu_relax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    __builtin_ia32_pause();
#endif
}
}  // namespace

void srw_lock_exclusive_slow(SRWLOCK* l) {
    for (int spin = 0;; ++spin) {
        uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if ((s & ~kSrwP) == 0) {
            if (__atomic_compare_exchange_n(&l->state, &s, kSrwW, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < 64) { cpu_relax(); continue; }
        if (!(s & kSrwP)) {                                    // queue new readers behind this writer
            if (!__atomic_compare_exchange_n(&l->state, &s, s | kSrwP, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
            s |= kSrwP;
        }
        __atomic_fetch_add(&l->waiters, 1u, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&l->state, __ATOMIC_SEQ_CST) == s) os_sync_wait_on_address(&l->state, s, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        __atomic_fetch_sub(&l->waiters, 1u, __ATOMIC_SEQ_CST);
    }
}

void srw_lock_shared_slow(SRWLOCK* l) {
    for (int spin = 0;; ++spin) {
        uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(s & (kSrwW | kSrwP))) {
            if (__atomic_compare_exchange_n(&l->state, &s, s + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < 64) { cpu_relax(); continue; }
        __atomic_fetch_add(&l->waiters, 1u, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&l->state, __ATOMIC_SEQ_CST) == s) os_sync_wait_on_address(&l->state, s, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        __atomic_fetch_sub(&l->waiters, 1u, __ATOMIC_SEQ_CST);
    }
}

void srw_wake(SRWLOCK* l) { os_sync_wake_by_address_all(&l->state, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE); }

uint64_t thread_key();

}  // namespace nxwin

unsigned long GetLastError() { return nxwin::t_last_error; }
void SetLastError(DWORD e) { nxwin::t_last_error = e; }

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG flags) {
    const uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&c->waiters, 1u, __ATOMIC_SEQ_CST);
    const bool shared = flags & CONDITION_VARIABLE_LOCKMODE_SHARED;
    if (shared) ReleaseSRWLockShared(l);
    else ReleaseSRWLockExclusive(l);
    const bool woke = nxwin::wait_on_address(&c->seq, seq, 4, ms);
    __atomic_fetch_sub(&c->waiters, 1u, __ATOMIC_SEQ_CST);
    if (shared) AcquireSRWLockShared(l);
    else AcquireSRWLockExclusive(l);
    if (!woke) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

void InitializeCriticalSection(LPCRITICAL_SECTION cs) { std::memset(cs, 0, sizeof *cs); }
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD) { InitializeCriticalSection(cs); return TRUE; }
BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD, DWORD) { InitializeCriticalSection(cs); return TRUE; }
void DeleteCriticalSection(LPCRITICAL_SECTION) {}
void EnterCriticalSection(LPCRITICAL_SECTION cs) {
    const uint64_t me = nxwin::thread_key();
    if (__atomic_load_n(&cs->owner, __ATOMIC_RELAXED) == me) { cs->recursion++; return; }
    AcquireSRWLockExclusive(&cs->lock);
    __atomic_store_n(&cs->owner, me, __ATOMIC_RELAXED);
    cs->recursion = 1;
}
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
    const uint64_t me = nxwin::thread_key();
    if (__atomic_load_n(&cs->owner, __ATOMIC_RELAXED) == me) { cs->recursion++; return TRUE; }
    if (!TryAcquireSRWLockExclusive(&cs->lock)) return FALSE;
    __atomic_store_n(&cs->owner, me, __ATOMIC_RELAXED);
    cs->recursion = 1;
    return TRUE;
}
void LeaveCriticalSection(LPCRITICAL_SECTION cs) {
    if (--cs->recursion > 0) return;
    __atomic_store_n(&cs->owner, 0, __ATOMIC_RELAXED);
    ReleaseSRWLockExclusive(&cs->lock);
}
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE c, PCRITICAL_SECTION cs, DWORD ms) {
    const LONG rec = cs->recursion;
    const uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&c->waiters, 1u, __ATOMIC_SEQ_CST);
    cs->recursion = 1;
    LeaveCriticalSection(cs);
    const bool woke = nxwin::wait_on_address(&c->seq, seq, 4, ms);
    __atomic_fetch_sub(&c->waiters, 1u, __ATOMIC_SEQ_CST);
    EnterCriticalSection(cs);
    cs->recursion = rec;
    if (!woke) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

// WaitOnAddress: 4- and 8-byte values wait on the address itself; 1- and 2-byte values on the aligned 4-byte word that holds them.
BOOL WaitOnAddress(volatile void* addr, PVOID compare, SIZE_T size, DWORD ms) {
    const uint64_t deadline = ms == INFINITE ? UINT64_MAX : nxwin::mono_ns() + static_cast<uint64_t>(ms) * 1000000ull;
    for (;;) {
        if (std::memcmp(const_cast<void*>(addr), compare, size) != 0) return TRUE;
        void* wa = const_cast<void*>(addr);
        size_t ws = size;
        uint64_t cur = 0;
        if (size == 8) cur = __atomic_load_n(static_cast<uint64_t*>(wa), __ATOMIC_SEQ_CST);
        else if (size == 4) cur = __atomic_load_n(static_cast<uint32_t*>(wa), __ATOMIC_SEQ_CST);
        else {
            wa = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) & ~uintptr_t{3});
            ws = 4;
            cur = __atomic_load_n(static_cast<uint32_t*>(wa), __ATOMIC_SEQ_CST);
            if (std::memcmp(const_cast<void*>(addr), compare, size) != 0) return TRUE;
        }
        DWORD left = INFINITE;
        if (deadline != UINT64_MAX) {
            const uint64_t now = nxwin::mono_ns();
            if (now >= deadline) { SetLastError(ERROR_TIMEOUT); return FALSE; }
            left = static_cast<DWORD>(std::max<uint64_t>(1, (deadline - now) / 1000000ull));
        }
        if (!nxwin::wait_on_address(wa, cur, ws, left)) {
            if (std::memcmp(const_cast<void*>(addr), compare, size) != 0) return TRUE;
            SetLastError(ERROR_TIMEOUT);
            return FALSE;
        }
        return TRUE;   // woken (maybe spuriously): Win32 callers re-check, as with WaitOnAddress itself
    }
}
static void* wake_word(PVOID addr) { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) & ~uintptr_t{3}); }
void WakeByAddressSingle(PVOID addr) {
    // The size is unknown here: wake both the address as an 8-byte and the 4-byte word (spurious wakes are allowed).
    if ((reinterpret_cast<uintptr_t>(addr) & 7) == 0) os_sync_wake_by_address_any(addr, 8, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    os_sync_wake_by_address_any(wake_word(addr), 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}
void WakeByAddressAll(PVOID addr) {
    if ((reinterpret_cast<uintptr_t>(addr) & 7) == 0) os_sync_wake_by_address_all(addr, 8, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    os_sync_wake_by_address_all(wake_word(addr), 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

// ---- time --------------------------------------------------------------------------------------------------------------------------------
namespace {
mach_timebase_info_data_t timebase() {
    static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
    return tb;
}
constexpr uint64_t kEpochDelta100ns = 116444736000000000ull;   // 1601-01-01 to 1970-01-01 in 100 ns units
void unix_ns_to_filetime(uint64_t ns, LPFILETIME ft) {
    const uint64_t v = ns / 100 + kEpochDelta100ns;
    ft->dwLowDateTime = static_cast<DWORD>(v);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
}
}  // namespace

BOOL QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = static_cast<LONGLONG>(mach_absolute_time()); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) {
    const auto tb = timebase();
    f->QuadPart = static_cast<LONGLONG>(1000000000ull * tb.denom / tb.numer);
    return TRUE;
}
ULONGLONG GetTickCount64() { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000ull; }
DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }
DWORD timeGetTime() { return GetTickCount(); }
UINT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
UINT timeEndPeriod(UINT) { return TIMERR_NOERROR; }
void GetSystemTimeAsFileTime(LPFILETIME ft) { unix_ns_to_filetime(clock_gettime_nsec_np(CLOCK_REALTIME), ft); }
void GetSystemTimePreciseAsFileTime(LPFILETIME ft) { GetSystemTimeAsFileTime(ft); }
BOOL QueryUnbiasedInterruptTime(PULONGLONG t) { *t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 100; return TRUE; }

static void tm_to_systemtime(const struct tm& t, unsigned ms, LPSYSTEMTIME st) {
    st->wYear = static_cast<WORD>(t.tm_year + 1900);
    st->wMonth = static_cast<WORD>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(t.tm_wday);
    st->wDay = static_cast<WORD>(t.tm_mday);
    st->wHour = static_cast<WORD>(t.tm_hour);
    st->wMinute = static_cast<WORD>(t.tm_min);
    st->wSecond = static_cast<WORD>(t.tm_sec);
    st->wMilliseconds = static_cast<WORD>(ms);
}
void GetSystemTime(LPSYSTEMTIME st) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    gmtime_r(&tv.tv_sec, &t);
    tm_to_systemtime(t, static_cast<unsigned>(tv.tv_usec / 1000), st);
}
void GetLocalTime(LPSYSTEMTIME st) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    tm_to_systemtime(t, static_cast<unsigned>(tv.tv_usec / 1000), st);
}
BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st) {
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    if (v < kEpochDelta100ns) return FALSE;
    const uint64_t u = v - kEpochDelta100ns;
    const time_t s = static_cast<time_t>(u / 10000000ull);
    struct tm t;
    gmtime_r(&s, &t);
    tm_to_systemtime(t, static_cast<unsigned>((u / 10000ull) % 1000ull), st);
    return TRUE;
}
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft) {
    struct tm t{};
    t.tm_year = st->wYear - 1900;
    t.tm_mon = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min = st->wMinute;
    t.tm_sec = st->wSecond;
    const time_t s = timegm(&t);
    unix_ns_to_filetime(static_cast<uint64_t>(s) * 1000000000ull + st->wMilliseconds * 1000000ull, ft);
    return TRUE;
}
BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local) {
    const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    const time_t s = static_cast<time_t>((v - kEpochDelta100ns) / 10000000ull);
    struct tm t;
    localtime_r(&s, &t);
    const uint64_t r = v + static_cast<uint64_t>(static_cast<int64_t>(t.tm_gmtoff) * 10000000ll);
    local->dwLowDateTime = static_cast<DWORD>(r);
    local->dwHighDateTime = static_cast<DWORD>(r >> 32);
    return TRUE;
}

// ---- strings: UTF-8 <-> wchar_t (UTF-32) ---------------------------------------------------------------------------------------------
namespace {
// Decodes one UTF-8 sequence; returns its length (>= 1) and the code point (U+FFFD for a malformed sequence).
size_t utf8_decode(const unsigned char* s, size_t n, uint32_t* cp) {
    const unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (!len || len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0x7F >> len);
    for (size_t i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v <= 0xDFFF))
        v = 0xFFFD;
    *cp = v;
    return len;
}
size_t utf8_encode(uint32_t cp, char* out) {
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = static_cast<char>(cp); return 1; }
    if (cp < 0x800) { out[0] = static_cast<char>(0xC0 | (cp >> 6)); out[1] = static_cast<char>(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12)); out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x10000) return utf8_encode(cp, out);
    out[0] = static_cast<char>(0xF0 | (cp >> 18)); out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}
}  // namespace

int MultiByteToWideChar(UINT, DWORD, LPCSTR src, int n, LPWSTR dst, int cap) {
    if (!src) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const size_t len = n < 0 ? std::strlen(src) + 1 : static_cast<size_t>(n);
    const auto* s = reinterpret_cast<const unsigned char*>(src);
    int out = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += utf8_decode(s + i, len - i, &cp);
        if (cap > 0) {
            if (out >= cap) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            dst[out] = static_cast<wchar_t>(cp);
        }
        ++out;
    }
    return out;
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR src, int n, LPSTR dst, int cap, LPCSTR, LPBOOL used_def) {
    if (used_def) *used_def = FALSE;
    if (!src) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const size_t len = n < 0 ? wcslen(src) + 1 : static_cast<size_t>(n);
    int out = 0;
    char b[4];
    for (size_t i = 0; i < len; ++i) {
        const size_t k = utf8_encode(static_cast<uint32_t>(src[i]), b);
        if (cap > 0) {
            if (out + static_cast<int>(k) > cap) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            std::memcpy(dst + out, b, k);
        }
        out += static_cast<int>(k);
    }
    return out;
}

namespace nxwin {
std::string narrow(LPCWSTR w) {
    if (!w) return {};
    std::string s;
    char b[4];
    for (; *w; ++w) s.append(b, utf8_encode(static_cast<uint32_t>(*w), b));
    return s;
}
std::wstring widen(const char* s) {
    if (!s) return {};
    std::wstring w;
    const size_t n = std::strlen(s);
    const auto* u = reinterpret_cast<const unsigned char*>(s);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(u + i, n - i, &cp);
        w.push_back(static_cast<wchar_t>(cp));
    }
    return w;
}
void* aligned_malloc(size_t size, size_t align) {
    void* p = nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    return posix_memalign(&p, align, size ? size : 1) == 0 ? p : nullptr;
}
}  // namespace nxwin

// ---- environment --------------------------------------------------------------------------------------------------------------------------
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n) {
    const char* v = getenv(name);
    if (!v) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
    const size_t len = std::strlen(v);
    if (!buf || n <= len) return static_cast<DWORD>(len + 1);
    std::memcpy(buf, v, len + 1);
    return static_cast<DWORD>(len);
}
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD n) {
    const char* v = getenv(nxwin::narrow(name).c_str());
    if (!v) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
    const std::wstring w = nxwin::widen(v);
    if (!buf || n <= w.size()) return static_cast<DWORD>(w.size() + 1);
    std::wmemcpy(buf, w.c_str(), w.size() + 1);
    return static_cast<DWORD>(w.size());
}
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) { return (value ? setenv(name, value, 1) : unsetenv(name)) == 0; }
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
    const std::string n = nxwin::narrow(name);
    return (value ? setenv(n.c_str(), nxwin::narrow(value).c_str(), 1) : unsetenv(n.c_str())) == 0;
}
DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD n) {
    std::string out;
    for (const char* p = src; *p;) {
        if (*p == '%') {
            const char* e = std::strchr(p + 1, '%');
            if (e) {
                const std::string name(p + 1, e);
                const char* v = getenv(name.c_str());
                out += v ? v : std::string(p, e + 1);
                p = e + 1;
                continue;
            }
        }
        out += *p++;
    }
    if (dst && n > out.size()) std::memcpy(dst, out.c_str(), out.size() + 1);
    return static_cast<DWORD>(out.size() + 1);
}

namespace {
std::string g_cmdline;
std::wstring g_cmdline_w;
std::once_flag g_cmdline_once;
void build_cmdline() {
    std::call_once(g_cmdline_once, [] {
        int argc = *_NSGetArgc();
        char** argv = *_NSGetArgv();
        for (int i = 0; i < argc; ++i) {
            if (i) g_cmdline += ' ';
            const bool q = std::strchr(argv[i], ' ') != nullptr;
            if (q) g_cmdline += '"';
            g_cmdline += argv[i];
            if (q) g_cmdline += '"';
        }
        g_cmdline_w = nxwin::widen(g_cmdline.c_str());
    });
}
}  // namespace
LPSTR GetCommandLineA() { build_cmdline(); return g_cmdline.data(); }
LPWSTR GetCommandLineW() { build_cmdline(); return g_cmdline_w.data(); }
LPWSTR* CommandLineToArgvW(LPCWSTR cmd, int* argc) {
    std::vector<std::wstring> args;
    std::wstring cur;
    bool q = false, any = false;
    for (const wchar_t* p = cmd; p && *p; ++p) {
        if (*p == L'"') { q = !q; any = true; continue; }
        if (!q && (*p == L' ' || *p == L'\t')) {
            if (any || !cur.empty()) args.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += *p;
    }
    if (any || !cur.empty()) args.push_back(cur);
    size_t bytes = sizeof(LPWSTR) * (args.size() + 1);
    for (auto& a : args) bytes += (a.size() + 1) * sizeof(wchar_t);
    auto** out = static_cast<LPWSTR*>(std::malloc(bytes));
    auto* text = reinterpret_cast<wchar_t*>(out + args.size() + 1);
    for (size_t i = 0; i < args.size(); ++i) {
        out[i] = text;
        std::wmemcpy(text, args[i].c_str(), args[i].size() + 1);
        text += args[i].size() + 1;
    }
    out[args.size()] = nullptr;
    *argc = static_cast<int>(args.size());
    return out;
}
HLOCAL LocalFree(HLOCAL p) { std::free(p); return nullptr; }

// Private profile (ini) functions: a small reader/writer of [section] key=value files.
namespace {
std::vector<std::string> read_lines(const char* file) {
    std::vector<std::string> lines;
    FILE* f = std::fopen(file, "rb");
    if (!f) return lines;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        lines.push_back(s);
    }
    std::fclose(f);
    return lines;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}
bool ieq(const std::string& a, const char* b) { return strcasecmp(a.c_str(), b) == 0; }
}  // namespace
DWORD GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file) {
    std::string val = def ? def : "";
    bool in = false;
    for (auto& l : read_lines(file)) {
        const std::string t = trim(l);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') { in = t.size() > 2 && ieq(trim(t.substr(1, t.find(']') - 1)), sec); continue; }
        const size_t eq = t.find('=');
        if (in && eq != std::string::npos && ieq(trim(t.substr(0, eq)), key)) { val = trim(t.substr(eq + 1)); break; }
    }
    if (!out || !n) return 0;
    const size_t k = std::min<size_t>(val.size(), n - 1);
    std::memcpy(out, val.data(), k);
    out[k] = 0;
    return static_cast<DWORD>(k);
}
UINT GetPrivateProfileIntA(LPCSTR sec, LPCSTR key, INT def, LPCSTR file) {
    char b[64];
    if (!GetPrivateProfileStringA(sec, key, "", b, sizeof b, file) || !b[0]) return static_cast<UINT>(def);
    return static_cast<UINT>(std::strtol(b, nullptr, 0));
}
BOOL WritePrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR value, LPCSTR file) {
    auto lines = read_lines(file);
    int sec_at = -1, key_at = -1, sec_end = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string t = trim(lines[static_cast<size_t>(i)]);
        if (!t.empty() && t[0] == '[') {
            if (sec_at >= 0 && sec_end < 0) sec_end = i;
            if (ieq(trim(t.substr(1, t.find(']') - 1)), sec)) sec_at = i;
            continue;
        }
        const size_t eq = t.find('=');
        if (sec_at >= 0 && sec_end < 0 && key && eq != std::string::npos && ieq(trim(t.substr(0, eq)), key)) key_at = i;
    }
    if (sec_at >= 0 && sec_end < 0) sec_end = static_cast<int>(lines.size());
    const std::string kv = key ? std::string(key) + "=" + (value ? value : "") : std::string();
    if (key_at >= 0) {
        if (value) lines[static_cast<size_t>(key_at)] = kv;
        else lines.erase(lines.begin() + key_at);
    } else if (key && value) {
        if (sec_at < 0) {
            lines.push_back(std::string("[") + sec + "]");
            lines.push_back(kv);
        } else {
            lines.insert(lines.begin() + sec_end, kv);
        }
    }
    FILE* f = std::fopen(file, "wb");
    if (!f) { SetLastError(nxwin::errno_to_win32(errno)); return FALSE; }
    for (auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
    return TRUE;
}

// ---- modules ------------------------------------------------------------------------------------------------------------------------------
namespace {
// Windows system DLLs have no counterpart: loading one fails, as on a Windows without that component.
bool is_windows_dll(const std::string& n) {
    std::string l = n;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.size() > 4 && l.compare(l.size() - 4, 4, ".dll") == 0;
}
void* main_module() {
    static void* h = dlopen(nullptr, RTLD_NOW);
    return h;
}
}  // namespace
HMODULE LoadLibraryA(LPCSTR name) { return LoadLibraryExA(name, nullptr, 0); }
HMODULE LoadLibraryW(LPCWSTR name) { return LoadLibraryExA(nxwin::narrow(name).c_str(), nullptr, 0); }
HMODULE LoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags) { return LoadLibraryExA(nxwin::narrow(name).c_str(), f, flags); }
HMODULE LoadLibraryExA(LPCSTR name, HANDLE, DWORD) {
    if (!name || is_windows_dll(name)) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!h) SetLastError(ERROR_MOD_NOT_FOUND);
    return h;
}
BOOL FreeLibrary(HMODULE m) { return m && dlclose(m) == 0; }
FARPROC GetProcAddress(HMODULE m, LPCSTR name) {
    if (!m || !name) { SetLastError(ERROR_PROC_NOT_FOUND); return nullptr; }
    void* p = dlsym(m, name);
    if (!p) SetLastError(ERROR_PROC_NOT_FOUND);
    return reinterpret_cast<FARPROC>(p);
}
HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name) return main_module();
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
HMODULE GetModuleHandleW(LPCWSTR name) { return name ? GetModuleHandleA(nxwin::narrow(name).c_str()) : main_module(); }
// FROM_ADDRESS: the handle is the image's mach header (dladdr's dli_fbase), which GetModuleFileName understands.
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        Dl_info di;
        if (!dladdr(name, &di) || !di.dli_fbase) { SetLastError(ERROR_MOD_NOT_FOUND); *out = nullptr; return FALSE; }
        *out = di.dli_fbase;
        return TRUE;
    }
    *out = GetModuleHandleA(name);
    return *out != nullptr;
}
BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) return GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(name), out);
    return GetModuleHandleExA(flags, name ? nxwin::narrow(name).c_str() : nullptr, out);
}
namespace nxwin {
std::string module_path(HMODULE m) {
    if (m && m != main_module()) {
        Dl_info di;
        if (dladdr(m, &di) && di.dli_fname) {
            char real[PATH_MAX];
            return realpath(di.dli_fname, real) ? real : di.dli_fname;
        }
    }
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string p(n, '\0');
    if (_NSGetExecutablePath(p.data(), &n) != 0) return {};
    p.resize(std::strlen(p.c_str()));
    char real[PATH_MAX];
    return realpath(p.c_str(), real) ? real : p;
}
}  // namespace nxwin
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    const std::string p = nxwin::module_path(m);
    if (!n) return 0;
    const size_t k = std::min<size_t>(p.size(), n - 1);
    std::memcpy(buf, p.data(), k);
    buf[k] = 0;
    if (p.size() >= n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return n; }
    SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(k);
}
DWORD GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD n) {
    const std::wstring p = nxwin::widen(nxwin::module_path(m).c_str());
    if (!n) return 0;
    const size_t k = std::min<size_t>(p.size(), n - 1);
    std::wmemcpy(buf, p.data(), k);
    buf[k] = 0;
    if (p.size() >= n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return n; }
    SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(k);
}
BOOL SetDllDirectoryW(LPCWSTR) { return TRUE; }

// ---- processor information ------------------------------------------------------------------------------------------------------------------
namespace {
struct Topology { int p = 0, e = 0; };
int sysctl_int(const char* name, int def) {
    int v = 0;
    size_t len = sizeof v;
    return sysctlbyname(name, &v, &len, nullptr, 0) == 0 ? v : def;
}
Topology topology() {
    static Topology t = [] {
        Topology r;
        const int levels = sysctl_int("hw.nperflevels", 1);
        r.p = sysctl_int("hw.perflevel0.logicalcpu", sysctl_int("hw.logicalcpu", 1));
        r.e = levels > 1 ? sysctl_int("hw.perflevel1.logicalcpu", 0) : 0;
        if (r.p + r.e > 64) r.e = std::max(0, 64 - r.p);
        return r;
    }();
    return t;
}
}  // namespace
BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf, PDWORD len) {
    const Topology t = topology();
    const int n = t.p + t.e;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX> recs;
    if (rel == RelationProcessorCore || rel == RelationAll) {
        for (int i = 0; i < n; ++i) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX r{};
            r.Relationship = RelationProcessorCore;
            r.Size = sizeof r;
            r.Processor.EfficiencyClass = i < t.p ? 1 : 0;
            r.Processor.GroupCount = 1;
            r.Processor.GroupMask[0].Mask = KAFFINITY{1} << i;
            recs.push_back(r);
        }
    }
    if (rel == RelationGroup || rel == RelationAll) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX r{};
        r.Relationship = RelationGroup;
        r.Size = sizeof r;
        r.Group.MaximumGroupCount = r.Group.ActiveGroupCount = 1;
        r.Group.GroupInfo[0].MaximumProcessorCount = r.Group.GroupInfo[0].ActiveProcessorCount = static_cast<BYTE>(n);
        r.Group.GroupInfo[0].ActiveProcessorMask = n >= 64 ? ~KAFFINITY{0} : (KAFFINITY{1} << n) - 1;
        recs.push_back(r);
    }
    const DWORD need = static_cast<DWORD>(recs.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX));
    if (!buf || *len < need) {
        *len = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(buf, recs.data(), need);
    *len = need;
    return TRUE;
}
DWORD GetCurrentProcessorNumber() { return 0; }
void GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER n) { n->Group = 0; n->Number = 0; n->Reserved = 0; }
BOOL SetThreadIdealProcessorEx(HANDLE, PPROCESSOR_NUMBER, PPROCESSOR_NUMBER prev) {
    if (prev) std::memset(prev, 0, sizeof *prev);
    return TRUE;
}
BOOL GetThreadIdealProcessorEx(HANDLE, PPROCESSOR_NUMBER ideal) { std::memset(ideal, 0, sizeof *ideal); return TRUE; }
DWORD SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }
BOOL SetThreadSelectedCpuSets(HANDLE, const ULONG*, ULONG) { return TRUE; }
DWORD GetActiveProcessorCount(WORD) { const Topology t = topology(); return static_cast<DWORD>(t.p + t.e); }
BOOL SetThreadInformation(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD) { return TRUE; }
BOOL SetProcessInformation(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD) { return TRUE; }

// ---- locale -------------------------------------------------------------------------------------------------------------------------------
namespace {
std::string preferred_language() {
    std::string out = "en-US";
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if (langs && CFArrayGetCount(langs) > 0) {
        auto s = static_cast<CFStringRef>(CFArrayGetValueAtIndex(langs, 0));
        char b[64];
        if (CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8)) out = b;
    }
    if (langs) CFRelease(langs);
    return out;
}
struct LangMap { const char* prefix; LANGID id; };
// Longest prefixes first: "zh-Hant" before "zh", "pt-BR" before "pt".
const LangMap kLangs[] = {
    {"zh-Hant", 0x0404}, {"zh-TW", 0x0404}, {"zh-HK", 0x0C04}, {"zh", 0x0804}, {"ja", 0x0411}, {"ko", 0x0412},
    {"en-GB", 0x0809}, {"en-AU", 0x0C09}, {"en", 0x0409}, {"fr-CA", 0x0C0C}, {"fr", 0x040C}, {"de", 0x0407}, {"it", 0x0410},
    {"es-419", 0x580A}, {"es-MX", 0x080A}, {"es", 0x0C0A}, {"nl", 0x0413}, {"pt-BR", 0x0416}, {"pt", 0x0816}, {"ru", 0x0419},
    {"pl", 0x0415}, {"ar", 0x0401}, {"hi", 0x0439}, {"tr", 0x041F}, {"sv", 0x041D}, {"da", 0x0406}, {"nb", 0x0414}, {"fi", 0x040B},
};
}  // namespace
LANGID GetUserDefaultUILanguage() {
    const std::string l = preferred_language();
    for (const auto& m : kLangs)
        if (l.compare(0, std::strlen(m.prefix), m.prefix) == 0) return m.id;
    return 0x0409;
}
LANGID GetUserDefaultLangID() { return GetUserDefaultUILanguage(); }
LANGID GetSystemDefaultUILanguage() { return GetUserDefaultUILanguage(); }
int GetUserDefaultLocaleName(LPWSTR buf, int n) {
    const std::wstring w = nxwin::widen(preferred_language().c_str());
    if (!buf || n <= static_cast<int>(w.size())) return 0;
    std::wmemcpy(buf, w.c_str(), w.size() + 1);
    return static_cast<int>(w.size() + 1);
}

// ---- user interface and shell -----------------------------------------------------------------------------------------------------------
int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    std::fprintf(stderr, "%s: %s\n", caption ? caption : "", text ? text : "");
    CFStringRef t = CFStringCreateWithCString(nullptr, caption ? caption : "", kCFStringEncodingUTF8);
    CFStringRef m = CFStringCreateWithCString(nullptr, text ? text : "", kCFStringEncodingUTF8);
    CFOptionFlags flags = (type & 0xF0) == MB_ICONERROR ? kCFUserNotificationStopAlertLevel
                          : (type & 0xF0) == MB_ICONWARNING ? kCFUserNotificationCautionAlertLevel : kCFUserNotificationNoteAlertLevel;
    const bool two = (type & 0xF) == MB_OKCANCEL || (type & 0xF) == MB_YESNO;
    CFStringRef ok = (type & 0xF) == MB_YESNO ? CFSTR("Yes") : nullptr;
    CFStringRef alt = two ? ((type & 0xF) == MB_YESNO ? CFSTR("No") : CFSTR("Cancel")) : nullptr;
    CFOptionFlags resp = kCFUserNotificationDefaultResponse;
    CFUserNotificationDisplayAlert(0, flags, nullptr, nullptr, nullptr, t, m, ok, alt, nullptr, &resp);
    if (t) CFRelease(t);
    if (m) CFRelease(m);
    if ((type & 0xF) == MB_YESNO) return resp == kCFUserNotificationDefaultResponse ? IDYES : IDNO;
    if ((type & 0xF) == MB_OKCANCEL) return resp == kCFUserNotificationDefaultResponse ? IDOK : IDCANCEL;
    return IDOK;
}
int MessageBoxW(HWND w, LPCWSTR text, LPCWSTR caption, UINT type) {
    return MessageBoxA(w, nxwin::narrow(text).c_str(), nxwin::narrow(caption).c_str(), type);
}
// "open" (or no verb) on a file, folder or URL: /usr/bin/open. Returns a value > 32 on success, as ShellExecute does.
HINSTANCE ShellExecuteA(HWND, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR, INT) {
    if (!file || (op && std::strcmp(op, "open") != 0 && std::strcmp(op, "explore") != 0)) return reinterpret_cast<HINSTANCE>(2);
    std::vector<std::string> args = {"/usr/bin/open", file};
    if (params && *params) { args.push_back("--args"); args.push_back(params); }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return reinterpret_cast<HINSTANCE>(2);
    return reinterpret_cast<HINSTANCE>(33);
}
HINSTANCE ShellExecuteW(HWND w, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show) {
    const std::string o = nxwin::narrow(op), f = nxwin::narrow(file), p = nxwin::narrow(params), d = nxwin::narrow(dir);
    return ShellExecuteA(w, op ? o.c_str() : nullptr, f.c_str(), params ? p.c_str() : nullptr, dir ? d.c_str() : nullptr, show);
}

// ---- console control handler: SIGINT / SIGTERM / SIGHUP call the handlers, newest first, as CTRL_C / CTRL_CLOSE ----
namespace {
std::mutex g_ctrl_m;
std::vector<PHANDLER_ROUTINE> g_ctrl;
void on_ctrl(int sig) {
    const DWORD ev = sig == SIGINT ? CTRL_C_EVENT : CTRL_CLOSE_EVENT;
    std::vector<PHANDLER_ROUTINE> hs;
    {
        std::lock_guard<std::mutex> g(g_ctrl_m);
        hs = g_ctrl;
    }
    for (auto it = hs.rbegin(); it != hs.rend(); ++it)
        if ((*it)(ev)) return;
    _exit(sig == SIGINT ? 130 : 143);
}
}  // namespace
BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE h, BOOL add) {
    std::lock_guard<std::mutex> g(g_ctrl_m);
    if (!h) return TRUE;
    if (add) {
        if (g_ctrl.empty()) {
            signal(SIGINT, on_ctrl);
            signal(SIGTERM, on_ctrl);
            signal(SIGHUP, on_ctrl);
        }
        g_ctrl.push_back(h);
    } else {
        g_ctrl.erase(std::remove(g_ctrl.begin(), g_ctrl.end(), h), g_ctrl.end());
    }
    return TRUE;
}
