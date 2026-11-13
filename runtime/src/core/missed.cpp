// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Code the lift did not see (nxrt/missed.h; docs/ARCHITECTURE.md): mid-function entries through the build's tables, units recompiled on
// demand by the build's own lifter and toolchain, the title's cache of those units, and missed.txt for the next build.
#include "nxrt/missed.h"

#include <windows.h>

#if !defined(_WIN32)
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nxrecomp/ondemand.h>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"

void nx_catch_call_at(Ctx* c, void (*body)(Ctx*, uint64_t), uint64_t pc);   // eh/catch_call.cpp

namespace nxrt::missed {
namespace {

namespace fs = std::filesystem;
constexpr const char* kTag = "missed";
using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
constexpr const char* kUnitExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kUnitExt = ".dylib";    // nxwin's LoadLibrary refuses a ".dll" name off Windows
#else
constexpr const char* kUnitExt = ".so";
#endif

fs::path P(const std::string& s) {
#if defined(_WIN32)
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), static_cast<int>(w.size()));
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return fs::path(w);
#else
    return fs::path(s);
#endif
}
std::string U(const fs::path& p) {
#if defined(_WIN32)
    const std::wstring w = p.wstring();
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    if (!s.empty()) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
#else
    return p.string();
#endif
}
bool exists(const std::string& s) {
    std::error_code ec;
    return !s.empty() && fs::exists(P(s), ec);
}
std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}
std::string exe_dir() {
    char b[4096];
    const DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
    std::string s(b, n < sizeof b ? n : 0);
    const size_t k = s.find_last_of("/\\");
    return k == std::string::npos ? std::string(".") : s.substr(0, k);
}
std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = line.find('\t', a);
        out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    for (auto& s : out)
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return out;
}

// ---- probing the entry switch of a lifted function ------------------------------------------------------------------------------
// u_<entry>(c, addr) jumps to addr when it is one of its entries and otherwise calls nx_no_entry(c, addr) before running any guest
// instruction. The probe marks the thread so nx_no_entry knows the call is a question, not a bad resume. Nested probes (the entered code
// meets another unknown address) save and restore the mark.
thread_local uint64_t t_probe = 0;
thread_local bool t_probe_failed = false;

bool probe(::Ctx* c, void (*fn)(::Ctx*, uint64_t), uint32_t flags, uint64_t addr) {
    const uint64_t saved = t_probe;
    const bool saved_failed = t_probe_failed;
    const uint64_t saved_fn = c->fn;
    t_probe = addr;
    t_probe_failed = false;
    if (flags & NX_ENTRY_CATCHER) nx_catch_call_at(c, fn, addr);
    else fn(c, addr);
    const bool ok = !t_probe_failed;
    t_probe = saved;
    t_probe_failed = saved_failed;
    if (!ok) c->fn = saved_fn;              // the function only set c->fn before its switch said no
    return ok;
}

// the row whose range holds addr: the last one starting at or below it (rows nest when the build split a function: look back a little)
const NxEntryFn* row_for(const NxEntryFn* t, int n, uint64_t addr) {
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (t[mid].start <= addr) lo = mid + 1;
        else hi = mid;
    }
    for (int i = lo - 1, k = 0; i >= 0 && k < 16; i--, k++)
        if (addr < t[i].end && addr != t[i].start) return &t[i];
    return nullptr;
}

// Per-thread cache of entries that answered: a guest loop that enters the same address again skips the table searches and the probe.
std::atomic<uint32_t> g_hit_epoch{1};
struct Hit {
    uint64_t addr;
    void (*fn)(::Ctx*, uint64_t);
    uint32_t flags;
};
constexpr size_t kHits = 256;
thread_local Hit t_hits[kHits];
thread_local uint32_t t_hits_epoch = 0;
Hit& hit_slot(uint64_t addr) {
    const uint32_t ep = g_hit_epoch.load(std::memory_order_acquire);
    if (t_hits_epoch != ep) {
        std::memset(t_hits, 0, sizeof t_hits);
        t_hits_epoch = ep;
    }
    return t_hits[((addr >> 2) ^ (addr >> 11)) & (kHits - 1)];
}

std::atomic<uint64_t> g_static_entries{0}, g_unit_entries{0}, g_compiled{0}, g_from_cache{0}, g_failures{0}, g_recorded{0}, g_compile_ms{0};
std::atomic<uint64_t> g_dropped{0}, g_modules_compiled{0}, g_modules_from_cache{0};

bool enter_row(::Ctx* c, const NxEntryFn* r, uint64_t addr, std::atomic<uint64_t>& counter) {
    if (!r || !probe(c, r->fn, r->flags, addr)) return false;
    Hit& h = hit_slot(addr);
    h.addr = addr;
    h.fn = r->fn;
    h.flags = r->flags;
    if (counter.fetch_add(1, std::memory_order_relaxed) < 32)
        NXLOG_I(kTag, "entered lifted function 0x%llx in the middle, at 0x%llx", static_cast<unsigned long long>(r->start), static_cast<unsigned long long>(addr));
    return true;
}

// ---- units: loaded libraries of code recompiled on demand -------------------------------------------------------------------------
struct Unit {
    HMODULE lib = nullptr;
    const NxOdUnit* u = nullptr;
    std::string file;
    bool dropped = false;                      // the code it was made for was rewritten (code_region_invalidate): its rows are gone
};
std::mutex g_units_lock;
std::vector<std::unique_ptr<Unit>> g_units;    // never unloaded: their code may be on any thread's stack
std::vector<NxEntryFn> g_unit_rows;            // every unit's rows, sorted by start

// Every unit row whose range holds addr, nearest start first. A big function met at several addresses has one unit per address
// (ondemand.py: such a unit answers for its own address only), and every one of those units has a row over the whole function.
std::vector<NxEntryFn> unit_rows_at(uint64_t addr) {
    std::vector<NxEntryFn> out;
    std::lock_guard<std::mutex> lk(g_units_lock);
    const NxEntryFn* t = g_unit_rows.data();
    int lo = 0, hi = static_cast<int>(g_unit_rows.size());
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (t[mid].start <= addr) lo = mid + 1;
        else hi = mid;
    }
    for (int i = lo - 1, k = 0; i >= 0 && k < 64; i--, k++)
        if (addr < t[i].end && addr != t[i].start) out.push_back(t[i]);
    return out;
}

// True when a loaded unit was made for addr (the range it answers for holds addr): another thread made it while this one waited.
// A unit row over addr's function is not enough: that unit may answer for another address of the function only.
bool unit_answers(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_units_lock);
    for (const auto& rec : g_units)
        if (!rec->dropped && rec->u && rec->u->start <= addr && addr < rec->u->end) return true;
    return false;
}

bool enter_units(::Ctx* c, uint64_t addr) {
    if (GuestFn f = fn_lookup(addr)) {         // a unit registered addr itself (a new function, or m_<addr> = u_F(c, addr))
        f(c);
        return true;
    }
    for (const NxEntryFn& r : unit_rows_at(addr))   // the one whose switch has addr runs it; the others say no (probe)
        if (enter_row(c, &r, addr, g_unit_entries)) return true;
    return false;
}

const NxOdApi& api() {
    static NxOdApi a = [] {
        NxOdApi x{};
        x.size = sizeof x;
        x.abi = NXRECOMP_ABI_VERSION;
        x.call_indirect = ::call_indirect;
        x.host_import = ::host_import;
        x.guest_trap = ::guest_trap;
        x.guest_unimpl = ::guest_unimpl;
        x.guest_svc = ::guest_svc;
        x.guest_mrs = ::guest_mrs;
        x.guest_msr = ::guest_msr;
        x.nx_site_inline = ::nx_site_inline;
        x.nx_site_midhook = ::nx_site_midhook;
        x.nx_hook_entry = ::nx_hook_entry;
        x.stx_diag = ::stx_diag;
        x.nx_catch_call = ::nx_catch_call;
        x.nx_no_entry = ::nx_no_entry;
        return x;
    }();
    a.spin_hook = nx_spin_hook;               // set by the runtime after start-up: the value now
    return a;
}

// Loads a unit library and registers what it holds. Null with *err on failure.
const NxOdUnit* load_unit(const std::string& file, std::string* err) {
#if defined(_WIN32)
    HMODULE h = LoadLibraryW(P(file).wstring().c_str());
#else
    HMODULE h = LoadLibraryA(file.c_str());
#endif
    if (!h) {
        *err = "the library " + file + " does not load";
        return nullptr;
    }
    auto entry = reinterpret_cast<NxOdEntryFn>(GetProcAddress(h, NXOD_ENTRY));
    const NxOdUnit* u = entry ? entry(&api()) : nullptr;
    if (!u || u->version != NXOD_VERSION || u->abi != NXRECOMP_ABI_VERSION) {
        *err = "the library " + file + " is not a unit of this runtime (version or ABI)";
        return nullptr;                       // left loaded: harmless, and its code was never entered
    }
    for (int i = 0; i < u->nfns; i++) fn_register(u->fns[i].addr, u->fns[i].fn);
    std::lock_guard<std::mutex> lk(g_units_lock);
    for (int i = 0; i < u->nentries; i++) g_unit_rows.push_back(u->entries[i]);
    std::sort(g_unit_rows.begin(), g_unit_rows.end(), [](const NxEntryFn& a, const NxEntryFn& b) { return a.start < b.start; });
    auto rec = std::make_unique<Unit>();
    rec->lib = h;
    rec->u = u;
    rec->file = file;
    g_units.push_back(std::move(rec));
    return u;
}

// ---- the build's manifest -------------------------------------------------------------------------------------------------------
struct ModuleInfo {
    std::string name, build_id, map, input;
    uint64_t base = 0;
};
struct Manifest {
    bool present = false;
    std::string path, lifter_hash, cache, missed_file, log;
    std::vector<std::string> command;          // the helper and its fixed arguments (pkp_build.py --ondemand ...)
    std::vector<ModuleInfo> modules;
    const ModuleInfo* module(const std::string& name) const {
        for (const auto& m : modules)
            if (m.name == name) return &m;
        return nullptr;
    }
};
std::mutex g_mf_lock;
std::unique_ptr<Manifest> g_mf;

// Lines `key<TAB>value...`; `#` starts a comment line. Relative paths are relative to the manifest's folder.
const Manifest& manifest() {
    std::lock_guard<std::mutex> lk(g_mf_lock);
    if (g_mf) return *g_mf;
    g_mf = std::make_unique<Manifest>();
    Manifest& m = *g_mf;
    m.path = config_get_str("missed", "manifest", "");
    if (m.path.empty()) m.path = exe_dir() + "/build/missed/manifest.tsv";
    std::ifstream f(P(m.path));
    if (!f) return m;
    const fs::path dir = P(m.path).parent_path();
    auto abs = [&](const std::string& s) -> std::string {
        if (s.empty()) return s;
        const fs::path p = P(s);
        return U(p.is_absolute() ? p : dir / p);
    };
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto t = split_tabs(line);
        if (t[0] == "lifter_hash" && t.size() > 1) m.lifter_hash = t[1];
        else if (t[0] == "cache" && t.size() > 1) m.cache = abs(t[1]);
        else if (t[0] == "missed" && t.size() > 1) m.missed_file = abs(t[1]);
        else if (t[0] == "log" && t.size() > 1) m.log = abs(t[1]);
        else if (t[0] == "command") m.command.assign(t.begin() + 1, t.end());
        else if (t[0] == "command_path" && t.size() > 1) m.command.push_back(abs(t[1]));   // a path argument of the command
        else if (t[0] == "module" && t.size() >= 6) {
            ModuleInfo mi;
            mi.name = t[1];
            mi.base = std::strtoull(t[2].c_str(), nullptr, 16);
            mi.build_id = t[3];
            mi.map = abs(t[4]);
            mi.input = abs(t[5]);
            m.modules.push_back(mi);
        }
    }
    m.present = !m.command.empty() && !m.cache.empty() && !m.lifter_hash.empty();
    if (m.cache.empty()) m.cache = U(dir / "cache");
    NXLOG_I(kTag, "on-demand manifest %s: %s (%zu modules, lifter %s)", m.path.c_str(), m.present ? "ready" : "incomplete", m.modules.size(),
            m.lifter_hash.substr(0, 12).c_str());
    return m;
}

enum class Mode { Off, Record, Compile };
Mode mode() {
    const std::string s = config_get_str("missed", "mode", "");
#if defined(__APPLE__) && TARGET_OS_IPHONE
    if (s == "off") return Mode::Off;
    return Mode::Record;                       // iPhone / iPad cannot load new code
#else
    if (s == "off") return Mode::Off;
    if (s == "record") return Mode::Record;
    if (s == "compile") return Mode::Compile;
    return manifest().present ? Mode::Compile : Mode::Record;
#endif
}

// ---- missed.txt ----------------------------------------------------------------------------------------------------------------
std::mutex g_rec_lock;
std::set<std::pair<std::string, uint64_t>> g_rec_seen;
bool g_rec_loaded = false;

std::string missed_path() {
    std::string p = config_get_str("missed", "file", "");
    if (p.empty()) p = manifest().missed_file;
    if (p.empty()) p = exe_dir() + "/missed.txt";
    return p;
}

void record(const std::string& module, uint64_t off, const std::string& build_id, const char* how) {
    std::lock_guard<std::mutex> lk(g_rec_lock);
    const std::string path = missed_path();
    if (!g_rec_loaded) {
        g_rec_loaded = true;
        std::ifstream f(P(path));
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line.substr(0, line.find('#')));
            std::string name, offs;
            if (ss >> name >> offs) g_rec_seen.insert({name, std::strtoull(offs.c_str(), nullptr, 16)});
        }
    }
    if (!g_rec_seen.insert({module, off}).second) return;
    std::error_code ec;
    fs::create_directories(P(path).parent_path(), ec);
    std::ofstream f(P(path), std::ios::app);
    if (!f) {
        NXLOG_W(kTag, "cannot write %s: %s+0x%llx is not recorded for the next build", path.c_str(), module.c_str(), static_cast<unsigned long long>(off));
        return;
    }
    char date[32] = "";
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
    f << module << '\t' << hex(off) << "\t# build " << build_id.substr(0, 16) << ' ' << date << ' ' << how << '\n';
    g_recorded.fetch_add(1, std::memory_order_relaxed);
    NXLOG_I(kTag, "recorded %s+0x%llx in %s: the next build includes it", module.c_str(), static_cast<unsigned long long>(off), path.c_str());
}

// ---- where an address is ---------------------------------------------------------------------------------------------------------
struct Where {
    std::string module, build_id, map, input;
    uint64_t base = 0;
    bool code = false;                         // code made at run time (a code region), not a module
    uint64_t region_base = 0;
    uint64_t code_base = 0;                    // code regions: where `bytes` were read (the address met)
    std::vector<uint8_t> bytes;                // code regions: the bytes from code_base to at most 64 KiB on (the unit ends where its code does)
    std::vector<uint64_t> sites;               // make_code_unit: addresses that get an inline site (the cheats overlay's breakpoints)
};

struct Region {
    uint64_t base, size;
    std::string name;
};
std::mutex g_regions_lock;
std::vector<Region> g_regions;

uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

bool locate(uint64_t addr, Where* w) {
    if (const NxModule* nx = nxmodule_at(addr)) {
        w->module = nx->name;
        w->base = nx->base;
        if (Module* m = module_at(addr)) w->build_id = m->build_id();
        if (const ModuleInfo* mi = manifest().module(w->module)) {
            if (w->build_id.empty()) w->build_id = mi->build_id;
            w->map = mi->map;
            w->input = mi->input;
        }
        if (w->build_id.empty()) w->build_id = "unknown";
        return true;
    }
    std::lock_guard<std::mutex> lk(g_regions_lock);
    for (const Region& r : g_regions)
        if (addr - r.base < r.size) {
            // the function's bytes: from the region start to at most 64 KiB past the address (what follows is lifted when it is met)
            const uint64_t end = std::min(r.base + r.size, (addr & ~3ull) + 0x10000);
            w->code = true;
            w->module = r.name;
            w->region_base = r.base;
            w->code_base = addr & ~3ull;
            w->bytes.assign(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(w->code_base)), reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(end)));
            w->build_id = hex(fnv64(w->bytes.data(), w->bytes.size()));
            return true;
        }
    return false;
}

// ---- the cache ------------------------------------------------------------------------------------------------------------------
struct CacheRow {
    std::string module, build, lifter, file;
    uint64_t addr = 0, start = 0, end = 0;
};
std::mutex g_cache_lock;
std::vector<CacheRow> g_index;
bool g_index_loaded = false;

std::string index_path() { return manifest().cache + "/index.tsv"; }

void load_index() {
    if (g_index_loaded) return;
    g_index_loaded = true;
    std::ifstream f(P(index_path()));
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto t = split_tabs(line);
        if (t.size() < 7) continue;
        CacheRow r;
        r.module = t[0];
        r.build = t[1];
        r.lifter = t[2];
        r.addr = std::strtoull(t[3].c_str(), nullptr, 16);
        r.start = std::strtoull(t[4].c_str(), nullptr, 16);
        r.end = std::strtoull(t[5].c_str(), nullptr, 16);
        r.file = t[6];
        g_index.push_back(r);
    }
}

// A cached unit for addr: same module build and lifter, range holding addr, library present.
std::string cached(const Where& w, uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_cache_lock);
    load_index();
    const Manifest& m = manifest();
    for (auto it = g_index.rbegin(); it != g_index.rend(); ++it)
        if (it->module == w.module && it->build == w.build_id && it->lifter == m.lifter_hash && addr >= it->start && addr < it->end) {
            const std::string file = m.cache + "/" + it->file;
            if (exists(file)) return file;
        }
    return std::string();
}

void remember(const Where& w, const NxOdUnit* u, const std::string& file) {
    std::lock_guard<std::mutex> lk(g_cache_lock);
    load_index();
    CacheRow r;
    r.module = w.module;
    r.build = w.build_id;
    r.lifter = manifest().lifter_hash;
    r.addr = u->addr;
    r.start = u->start;
    r.end = u->end;
    r.file = U(P(file).filename());
    g_index.push_back(r);
    std::ofstream f(P(index_path()), std::ios::app);
    f << r.module << '\t' << r.build << '\t' << r.lifter << '\t' << hex(r.addr) << '\t' << hex(r.start) << '\t' << hex(r.end) << '\t' << r.file << '\n';
}

// ---- making a unit: the build's helper ------------------------------------------------------------------------------------------
#if defined(_WIN32)
std::wstring quote(const std::string& a) {
    std::wstring w = P(a).wstring();
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
    std::wstring q = L"\"";
    for (wchar_t ch : w) {
        if (ch == L'"') q += L'\\';
        q += ch;
    }
    return q + L"\"";
}
#endif

// Runs argv to the end (the calling guest thread waits here), telling the watchdog the process is busy, not frozen. -> exit code, -1
// when it did not start, -2 on the time limit.
int run(const std::vector<std::string>& argv, int64_t timeout_s) {
    const auto t0 = Clock::now();
    auto late = [&] { return timeout_s > 0 && Clock::now() - t0 > std::chrono::seconds(timeout_s); };
#if defined(_WIN32)
    std::wstring cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    int rc = -2;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            rc = static_cast<int>(code);
            break;
        }
        diag_progress();
        if (late()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    CloseHandle(pi.hProcess);
    return rc;
#else
    std::vector<char*> args;
    std::vector<std::string> copy(argv);
    for (auto& a : copy) args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return -1;
    for (;;) {
        int st = 0;
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        if (r < 0) return -1;
        diag_progress();
        if (late()) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return -2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}

thread_local std::string t_why;                // why the last enter() on this thread could not run the address (explain())

std::string unit_name(const Where& w, uint64_t addr) {
    std::string mod;
    for (char ch : w.module) mod += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
    return mod + "-" + w.build_id.substr(0, 16) + "-" + manifest().lifter_hash.substr(0, 12) + "-" + hex(addr) + kUnitExt;
}

// Makes (or finds in the cache) and loads the unit for addr. Called with g_make_lock held.
bool obtain(const Where& w, uint64_t addr) {
    const Manifest& m = manifest();
    const auto t0 = Clock::now();
    const unsigned long long off = static_cast<unsigned long long>(addr - (w.code ? w.region_base : w.base));
    std::string err;
    if (const std::string file = cached(w, addr); !file.empty()) {
        if (const NxOdUnit* u = load_unit(file, &err)) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            g_from_cache.fetch_add(1, std::memory_order_relaxed);
            NXLOG_I(kTag, "loaded 0x%llx (%s+0x%llx, the unit for 0x%llx..0x%llx) from the cache in %lld ms: %s", static_cast<unsigned long long>(addr),
                    w.module.c_str(), off, static_cast<unsigned long long>(u->start), static_cast<unsigned long long>(u->end), static_cast<long long>(ms),
                    file.c_str());
            return true;
        }
        NXLOG_W(kTag, "cached unit for 0x%llx not used: %s", static_cast<unsigned long long>(addr), err.c_str());
    }
    const std::string lib = m.cache + "/" + unit_name(w, addr);
    // A unit an earlier run made but never recorded: that process ended (a crash, its --max-seconds) while the build's helper was still
    // compiling, so the index has no row for it. Its name says the module build, the lifter and the address, and the helper writes it
    // whole (a rename), so it is this unit: loaded and recorded instead of compiled again. Star Fox's job worker met main+0xb8ab314 in
    // every run; each run waited 95 to 180 s for the same unit, longer than the game's own watchdog allows (74 s).
    if (exists(lib)) {
        if (const NxOdUnit* u = load_unit(lib, &err); u && addr >= u->start && addr < u->end) {
            remember(w, u, lib);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            g_from_cache.fetch_add(1, std::memory_order_relaxed);
            NXLOG_I(kTag, "loaded 0x%llx (%s+0x%llx, the unit for 0x%llx..0x%llx) from the cache in %lld ms, made by an earlier run that did not "
                    "record it: %s", static_cast<unsigned long long>(addr), w.module.c_str(), off, static_cast<unsigned long long>(u->start),
                    static_cast<unsigned long long>(u->end), static_cast<long long>(ms), lib.c_str());
            return true;
        }
        NXLOG_W(kTag, "unit %s for 0x%llx not used: %s", lib.c_str(), static_cast<unsigned long long>(addr),
                err.empty() ? "it does not hold that address" : err.c_str());
    }
    if (!w.code && (w.map.empty() || !exists(w.map))) {
        t_why = "the build left no lift map for module " + w.module;
        return false;
    }
    std::error_code ec;
    fs::create_directories(P(m.cache), ec);
    std::vector<std::string> argv = m.command;
    if (w.code) {
        const std::string bin = m.cache + "/code-" + w.build_id + ".bin";
        std::ofstream(P(bin), std::ios::binary).write(reinterpret_cast<const char*>(w.bytes.data()), static_cast<std::streamsize>(w.bytes.size()));
        argv.insert(argv.end(), {"--code", bin, "--base", hex(w.code_base), "--name", w.module});
        if (!w.sites.empty()) {
            const std::string sf = m.cache + "/code-" + w.build_id + ".sites";
            std::ofstream f(P(sf));
            for (uint64_t a : w.sites) f << "inline " << hex(a) << '\n';
            argv.insert(argv.end(), {"--sites", sf});
        }
    } else {
        argv.insert(argv.end(), {"--map", w.map});
        if (exists(w.input)) argv.insert(argv.end(), {"--input", w.input});
    }
    argv.insert(argv.end(), {"--addr", hex(addr), "--out", lib, "--log", m.log.empty() ? m.cache + "/ondemand.log" : m.log});
    NXLOG_I(kTag, "0x%llx (%s+0x%llx) is not in this build: recompiling it on demand (the thread waits)", static_cast<unsigned long long>(addr),
            w.module.c_str(), off);
    log_flush();
    const int rc = run(argv, config_get_int("missed", "timeout_s", 900));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    g_compile_ms.fetch_add(static_cast<uint64_t>(ms), std::memory_order_relaxed);
    if (rc != 0 || !exists(lib)) {
        g_failures.fetch_add(1, std::memory_order_relaxed);
        t_why = rc == -1 ? "the build's helper did not start (" + (argv.empty() ? std::string() : argv[0]) + ")"
                         : "the build's helper failed (exit " + std::to_string(rc) + ", see " + (m.log.empty() ? m.cache + "/ondemand.log" : m.log) + ")";
        NXLOG_E(kTag, "recompiling 0x%llx on demand failed after %lld ms: %s", static_cast<unsigned long long>(addr), static_cast<long long>(ms), t_why.c_str());
        return false;
    }
    const NxOdUnit* u = load_unit(lib, &err);
    if (!u) {
        g_failures.fetch_add(1, std::memory_order_relaxed);
        t_why = err;
        NXLOG_E(kTag, "recompiled 0x%llx but cannot use it: %s", static_cast<unsigned long long>(addr), err.c_str());
        return false;
    }
    remember(w, u, lib);
    g_compiled.fetch_add(1, std::memory_order_relaxed);
    const NxEntryFn* row = u->nentries ? &u->entries[0] : nullptr;
    NXLOG_I(kTag, "recompiled 0x%llx on demand in %lld ms (%s+0x%llx, function 0x%llx..0x%llx, the unit answers for 0x%llx..0x%llx): %s",
            static_cast<unsigned long long>(addr), static_cast<long long>(ms), w.module.c_str(), off,
            static_cast<unsigned long long>(row ? row->start : u->start), static_cast<unsigned long long>(row ? row->end : u->end),
            static_cast<unsigned long long>(u->start), static_cast<unsigned long long>(u->end), lib.c_str());
    return true;
}

std::mutex g_make_lock;                        // one unit at a time; a thread that meets an address being made waits, then enters it
std::set<uint64_t> g_failed;                   // addresses whose unit could not be made this run (not tried again); under g_make_lock

}  // namespace

bool enter(::Ctx* c, uint64_t addr) {
    if (addr & 3) return false;
    {
        Hit& h = hit_slot(addr);
        if (h.fn && h.addr == addr) {
            if (h.flags & NX_ENTRY_CATCHER) nx_catch_call_at(c, h.fn, addr);
            else h.fn(c, addr);
            return true;
        }
    }
    // 1. the build's own entry tables
    if (const NxModule* nx = nxmodule_at(addr)) {
        int n = 0;
        const NxEntryFn* t = nxmodule_entry_table(nx, &n);
        if (enter_row(c, row_for(t, n, addr), addr, g_static_entries)) return true;
    }
    // 2. units loaded before
    if (enter_units(c, addr)) return true;
    Where w;
    if (!locate(addr, &w)) {
        t_why.clear();
        return false;                          // not guest code: a null or stray pointer
    }
    const Mode md = mode();
    if (md == Mode::Off) {
        t_why = "[missed] mode = off";
        return false;
    }
    if (!w.code) record(w.module, addr - w.base, w.build_id, md == Mode::Compile ? "recompiled on demand" : "met at run time");
    if (md == Mode::Record) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
        t_why = "this device cannot load code made while the game runs";
#else
        t_why = manifest().present ? "[missed] mode = record" : "this build has no on-demand compiler (no " + manifest().path + ")";
#endif
        return false;
    }
    // 3. the cache, else compile; then enter through what was loaded
    {
        std::lock_guard<std::mutex> lk(g_make_lock);
        if (!fn_lookup(addr) && !unit_answers(addr)) {
            if (g_failed.count(addr)) {
                t_why = "recompiling it on demand failed earlier in this run";
                return false;
            }
            if (!obtain(w, addr)) {
                g_failed.insert(addr);
                return false;
            }
        }
    }
    return enter_units(c, addr);
}

bool prepare(uint64_t addr, std::string* err) {
    if (addr & 3) {
        if (err) *err = "not an instruction address";
        return false;
    }
    if (fn_lookup(addr) || unit_answers(addr)) return true;
    Where w;
    if (!locate(addr, &w)) {
        if (err) *err = "not guest code";
        return false;
    }
    const Mode md = mode();
    if (md != Mode::Compile) {
        if (err) *err = md == Mode::Off ? "[missed] mode = off" : "no on-demand compiler here";
        return false;
    }
    std::lock_guard<std::mutex> lk(g_make_lock);
    if (fn_lookup(addr) || unit_answers(addr)) return true;   // made meanwhile (a guest thread met it first)
    if (g_failed.count(addr)) {
        if (err) *err = "recompiling it on demand failed earlier in this run";
        return false;
    }
    if (!w.code) record(w.module, addr - w.base, w.build_id, "prepared for a code patch");
    if (!obtain(w, addr)) {
        g_failed.insert(addr);
        if (err) *err = t_why;
        return false;
    }
    return true;
}

bool code_region_of(uint64_t addr, uint64_t* base, uint64_t* end) {
    std::lock_guard<std::mutex> lk(g_regions_lock);
    for (const Region& r : g_regions)
        if (addr - r.base < r.size) {
            if (base) *base = r.base;
            if (end) *end = r.base + r.size;
            return true;
        }
    return false;
}

void code_region_add(uint64_t base, uint64_t size, const char* name) {
    std::lock_guard<std::mutex> lk(g_regions_lock);
    g_regions.push_back({base, size, name && *name ? name : "code"});
    NXLOG_I(kTag, "code region %s 0x%llx..0x%llx: code met there is recompiled from its bytes", g_regions.back().name.c_str(),
            static_cast<unsigned long long>(base), static_cast<unsigned long long>(base + size));
}

void code_region_remove(uint64_t base) {
    std::lock_guard<std::mutex> lk(g_regions_lock);
    g_regions.erase(std::remove_if(g_regions.begin(), g_regions.end(), [&](const Region& r) { return r.base == base; }), g_regions.end());
}

void code_region_invalidate(uint64_t addr, uint64_t size) {
    const uint64_t end = addr + size;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(g_units_lock);
        for (auto& rec : g_units) {
            const NxOdUnit* u = rec->u;
            if (rec->dropped || !u) continue;
            bool hit = false;
            for (int i = 0; i < u->nentries && !hit; i++) hit = u->entries[i].start < end && addr < u->entries[i].end;
            for (int i = 0; i < u->nfns && !hit; i++) hit = u->fns[i].addr - addr < size;
            if (!hit) continue;
            for (int i = 0; i < u->nfns; i++) fn_unregister(u->fns[i].addr);   // the unit's code stays loaded: a thread may still be in it
            rec->dropped = true;
            dropped++;
        }
        if (dropped) {
            g_unit_rows.clear();
            for (auto& rec : g_units)
                if (!rec->dropped)
                    for (int i = 0; i < rec->u->nentries; i++) g_unit_rows.push_back(rec->u->entries[i]);
            std::sort(g_unit_rows.begin(), g_unit_rows.end(), [](const NxEntryFn& a, const NxEntryFn& b) { return a.start < b.start; });
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_make_lock);
        for (auto it = g_failed.begin(); it != g_failed.end();) it = *it - addr < size ? g_failed.erase(it) : std::next(it);
    }
    g_hit_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (dropped) {
        g_dropped.fetch_add(dropped, std::memory_order_relaxed);
        NXLOG_I(kTag, "code at 0x%llx..0x%llx was rewritten: %zu unit(s) made for it dropped", static_cast<unsigned long long>(addr),
                static_cast<unsigned long long>(end), dropped);
    }
}

GuestFn make_code_unit(const std::string& name, uint64_t base, const std::vector<uint8_t>& bytes, const std::vector<uint64_t>& inline_sites,
                       std::string* err) {
    const Manifest& m = manifest();
    if (!m.present || mode() != Mode::Compile) {
        *err = m.present ? "[missed] mode is not compile" : "this build has no on-demand compiler (no " + m.path + ")";
        return nullptr;
    }
    if (bytes.empty() || (base & 3)) {
        *err = "no code to lift";
        return nullptr;
    }
    Where w;
    w.code = true;
    for (char ch : name) w.module += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
    if (w.module.empty()) w.module = "patch";
    w.region_base = base;
    w.code_base = base;
    w.bytes = bytes;
    w.sites = inline_sites;
    std::sort(w.sites.begin(), w.sites.end());
    // the cache key: the bytes and the sites (the same bytes with other breakpoints are another unit)
    uint64_t h = fnv64(bytes.data(), bytes.size());
    for (uint64_t a : w.sites) h = (h ^ a) * 0x100000001b3ull;
    w.build_id = hex(h);
    std::lock_guard<std::mutex> lk(g_make_lock);
    t_why.clear();
    if (!obtain(w, base)) {
        *err = t_why.empty() ? "the unit could not be made" : t_why;
        return nullptr;
    }
    // the unit that answers for base is the newest one made for these bytes: take its function at base
    std::lock_guard<std::mutex> lk2(g_units_lock);
    for (auto it = g_units.rbegin(); it != g_units.rend(); ++it) {
        const NxOdUnit* u = (*it)->u;
        if ((*it)->dropped || !u || u->addr != base) continue;
        for (int i = 0; i < u->nfns; i++)
            if (u->fns[i].addr == base) return u->fns[i].fn;
    }
    *err = "the unit has no function at its address";
    return nullptr;
}

void (*code_unit_resume(GuestFn fn))(::Ctx*, uint64_t) {
    std::lock_guard<std::mutex> lk(g_units_lock);
    for (auto it = g_units.rbegin(); it != g_units.rend(); ++it) {
        const NxOdUnit* u = (*it)->u;
        if ((*it)->dropped || !u) continue;
        for (int i = 0; i < u->nfns; i++) {
            if (u->fns[i].fn != fn) continue;
            for (int k = 0; k < u->nentries; k++)
                if (u->entries[k].start == u->fns[i].addr && u->entries[k].fn) return u->entries[k].fn;
            return nullptr;
        }
    }
    return nullptr;
}

const NxModule* load_module_unit(const char* name, uint64_t base, uint32_t index, const uint8_t* image, size_t size, std::string* err) {
    const Manifest& m = manifest();
    if (!m.present || mode() != Mode::Compile) {
        *err = m.present ? "[missed] mode is not compile" : "this build has no on-demand compiler (no " + m.path + ")";
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_make_lock);
    const auto t0 = Clock::now();
    std::string mod;
    for (const char* p = name; *p; p++) mod += (std::isalnum(static_cast<unsigned char>(*p)) ? *p : '_');
    const std::string key = mod + "-" + hex(fnv64(image, size)) + "-" + m.lifter_hash.substr(0, 12) + "-" + hex(base) + "-" + std::to_string(index);
    const std::string lib = m.cache + "/" + key + kUnitExt;
    const bool cached_lib = exists(lib);
    if (!cached_lib) {
        std::error_code ec;
        fs::create_directories(P(m.cache), ec);
        const std::string img = m.cache + "/" + key + ".nro";
        std::ofstream(P(img), std::ios::binary).write(reinterpret_cast<const char*>(image), static_cast<std::streamsize>(size));
        std::vector<std::string> argv = m.command;
        argv.insert(argv.end(), {"--image", img, "--base", hex(base), "--index", std::to_string(index), "--name", mod, "--out", lib, "--log",
                                 m.log.empty() ? m.cache + "/ondemand.log" : m.log});
        NXLOG_I(kTag, "module %s (%zu bytes at 0x%llx) is not in this build: recompiling it on demand (the thread waits)", name, size,
                static_cast<unsigned long long>(base));
        log_flush();
        const int rc = run(argv, config_get_int("missed", "timeout_s", 900));
        if (rc != 0 || !exists(lib)) {
            g_failures.fetch_add(1, std::memory_order_relaxed);
            *err = "the build's helper failed (exit " + std::to_string(rc) + ", see " + (m.log.empty() ? m.cache + "/ondemand.log" : m.log) + ")";
            NXLOG_E(kTag, "recompiling module %s on demand failed: %s", name, err->c_str());
            return nullptr;
        }
    }
#if defined(_WIN32)
    HMODULE h = LoadLibraryW(P(lib).wstring().c_str());
#else
    HMODULE h = LoadLibraryA(lib.c_str());
#endif
    auto entry = h ? reinterpret_cast<NxOdModuleEntryFn>(GetProcAddress(h, NXOD_MODULE_ENTRY)) : nullptr;
    const NxOdModuleUnit* u = entry ? entry(&api()) : nullptr;
    if (!u || u->version != NXOD_VERSION || !u->module) {
        *err = "the library " + lib + " is not a module unit of this runtime";
        return nullptr;
    }
    const RegResult r = nxmodule_register(nullptr, u->module, u->abi);
    if (!r.ok()) {
        *err = r.message;
        return nullptr;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    (cached_lib ? g_modules_from_cache : g_modules_compiled).fetch_add(1, std::memory_order_relaxed);
    NXLOG_I(kTag, "%s module %s (%zu functions at 0x%llx) %s in %lld ms: %s", cached_lib ? "loaded" : "recompiled", name, r.functions,
            static_cast<unsigned long long>(base), cached_lib ? "from the cache" : "on demand", static_cast<long long>(ms), lib.c_str());
    return u->module;
}

std::string explain(uint64_t addr) {
    Where w;
    if (addr & 3 || t_why.empty() || !locate(addr, &w)) return "indirect call to an address that is not a known function";
    char b[160];
    std::snprintf(b, sizeof b, "%s+0x%llx", w.module.c_str(), static_cast<unsigned long long>(addr - (w.code ? w.region_base : w.base)));
    if (w.code) return std::string("code made at run time at ") + b + " could not be recompiled: " + t_why;
    return std::string("the game ran code this build does not include (") + b + "): " + t_why + ". It is recorded in " + missed_path() +
           ", and the next build of the game includes it";
}

Stats stats() {
    Stats s;
    s.static_entries = g_static_entries.load();
    s.unit_entries = g_unit_entries.load();
    s.units_compiled = g_compiled.load();
    s.units_from_cache = g_from_cache.load();
    s.units_dropped = g_dropped.load();
    s.modules_compiled = g_modules_compiled.load();
    s.modules_from_cache = g_modules_from_cache.load();
    s.compile_failures = g_failures.load();
    s.recorded = g_recorded.load();
    s.compile_ms = g_compile_ms.load();
    return s;
}

void reset_for_tests() {
    {
        std::lock_guard<std::mutex> lk(g_units_lock);
        g_units.clear();                       // the libraries stay loaded (FreeLibrary is never called on a unit)
        g_unit_rows.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_mf_lock);
        g_mf.reset();
    }
    {
        std::lock_guard<std::mutex> lk(g_rec_lock);
        g_rec_seen.clear();
        g_rec_loaded = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_cache_lock);
        g_index.clear();
        g_index_loaded = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_make_lock);
        g_failed.clear();
    }
    g_hit_epoch.fetch_add(1);
    g_static_entries = g_unit_entries = g_compiled = g_from_cache = g_failures = g_recorded = g_compile_ms = 0;
    g_dropped = g_modules_compiled = g_modules_from_cache = 0;
}

}  // namespace nxrt::missed

// The default case of every entry switch (ctx.h, ABI 4): a probe of the dispatcher that the function answered "not an entry", else a
// resume at an address that is no entry of the function, a guest trap as before.
extern "C" void nx_no_entry(Ctx* c, uint64_t addr) {
    using namespace nxrt::missed;
    if (t_probe == addr && !t_probe_failed) {
        t_probe_failed = true;
        return;
    }
    nxrt::trap("resume at an address that is not an entry of this function", addr);
}
