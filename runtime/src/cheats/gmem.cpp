// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Guest memory for the cheat tools (gmem.h).
#include "gmem.h"

#include "nxrt/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_MSC_VER)
#define NXRT_NOINLINE_GUARD __declspec(noinline)
#else
#define NXRT_NOINLINE_GUARD __attribute__((noinline))
#endif

#include "nxrt/memory.h"
#include "nxrt/module.h"

namespace nxrt::cheats::gmem {
namespace {

// The address ranges the guest owns: the alias region, and the window from the main image to the end of the kernel region (images, the
// remote region, sdk modules, heap, stacks, TLS). Anything else is host memory.
bool in_window(uint64_t a, uint64_t n) {
    if (n == 0 || a + n < a) return false;
    const uint64_t e = a + n;
    if (a >= mem::kAliasBase && e <= mem::kAliasBase + mem::kAliasSize) return true;
    return a >= mem::kMainBase && e <= mem::kKernelBase + mem::kKernelSize;
}

// The guarded copy is a function of its own that does nothing after a fault but return false. Measured on clang-cl /EHa (2026-11-10,
// Wonder): inlined into write(), the __except path took the address and the source from rcx / rdx, which hold whatever the fault
// left there (gmem.cpp.obj: the handler at write+0xb6 stores rcx and rdx as the address and the source), so a code write the page
// refused went on to "write to 0x1a4640000 refused: not committed", a host page outside the guest's memory. Out of line, write()'s
// own values sit in its saved registers and stack, which the fault does not touch. (The CRT's memcpy_repmovs pushes rsi / rdi with
// unwind data, so the copy itself may stay memcpy.)
NXRT_NOINLINE_GUARD bool copy_guarded(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A write the guest's own protection refuses (main's code, read-only data): each page is made writable for the copy and put back. Only
// pages that are committed and readable are touched, never a free or no-access one.
bool write_protected(uint64_t addr, const void* src, size_t n) {
    if (!in_window(addr, n)) return false;   // never a host page
    const uint64_t ps = mem::host_page_size();
    const uint64_t first = addr & ~(ps - 1), last = (addr + n - 1) & ~(ps - 1);
    auto why = [](uint64_t p, const char* what, const MEMORY_BASIC_INFORMATION& mbi) {
        NXLOG_W("cheats", "write to 0x%llx refused: %s (state 0x%lx, type 0x%lx, protect 0x%lx, allocation protect 0x%lx, error %lu)",
                static_cast<unsigned long long>(p), what, static_cast<unsigned long>(mbi.State), static_cast<unsigned long>(mbi.Type),
                static_cast<unsigned long>(mbi.Protect), static_cast<unsigned long>(mbi.AllocationProtect), static_cast<unsigned long>(GetLastError()));
        return false;
    };
    for (uint64_t p = first; p <= last; p += ps) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) != sizeof mbi) return why(p, "VirtualQuery failed", mbi);
        if (mbi.State != MEM_COMMIT) return why(p, "not committed", mbi);
        const DWORD pr = mbi.Protect & 0xFF;
        if (pr == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return why(p, "no-access or guard page", mbi);
    }
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (uint64_t p = first; p <= last; p += ps) {
        const uint64_t lo = std::max(p, addr), hi = std::min(p + ps, addr + n);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(p), ps, PAGE_READWRITE, &old)) {
            const DWORD err = GetLastError();
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi);
            SetLastError(err);
            return why(p, "VirtualProtect to read-write failed", mbi);
        }
        const bool ok = copy_guarded(reinterpret_cast<void*>(lo), s + (lo - addr), static_cast<size_t>(hi - lo));
        DWORD dummy = 0;
        VirtualProtect(reinterpret_cast<void*>(p), ps, old, &dummy);
        if (!ok) return false;
    }
    return true;
}

}  // namespace

bool in_guest_range(uint64_t addr, uint64_t size) { return in_window(addr, size); }

bool read(uint64_t addr, void* dst, size_t n) {
    if (n == 0) return true;
    if (!in_window(addr, n)) return false;
    return copy_guarded(dst, reinterpret_cast<const void*>(addr), n);
}

bool write(uint64_t addr, const void* src, size_t n) {
    if (n == 0) return true;
    if (!in_window(addr, n)) return false;
    if (copy_guarded(reinterpret_cast<void*>(addr), src, n)) return true;
    return write_protected(addr, src, n);
}

bool with_mapped(uint64_t addr, size_t n, void (*fn)(const uint8_t* p, size_t n, void* user), void* user) {
    if (n == 0) return true;
    if (!in_window(addr, n)) return false;
    __try {
        fn(reinterpret_cast<const uint8_t*>(addr), n, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t main_base() {
    if (Module* m = module_by_name("main")) return m->base();
    return mem::kMainBase;
}

uint64_t main_size() {
    if (Module* m = module_by_name("main")) return m->size();
    return 0;
}

uint64_t region_base(unsigned t) {
    switch (t & 0xF) {
        case 1: return mem::kHeapBase;      // svcGetInfo 4, HeapRegionAddress: the heap grows from its start (SetHeapSize)
        case 2: return mem::kAliasBase;     // svcGetInfo 2, AliasRegionAddress
        case 3:                             // svcGetInfo 12, AslrRegionAddress (the same values as kernel/svc_info.cpp)
#if defined(__APPLE__)
            return mem::kGuestWindowLo;
#else
            return 0x0008000000ull;
#endif
        case 4: return 0;                   // absolute
        default: return main_base();        // 0 and 5..F: main
    }
}

bool module_of(uint64_t addr, std::string* name, uint64_t* base, uint64_t* size) {
    Module* m = module_at(addr);
    if (!m) return false;
    if (name) *name = m->name();
    if (base) *base = m->base();
    if (size) *size = m->size();
    return true;
}

std::string module_build_id(const std::string& name) {
    Module* m = module_by_name(name.c_str());
    return m ? m->build_id() : std::string();
}

std::string describe(uint64_t addr) {
    char b[96];
    std::string mod;
    uint64_t base = 0;
    if (module_of(addr, &mod, &base, nullptr)) {
        std::snprintf(b, sizeof b, "%s+0x%llx", mod.c_str(), static_cast<unsigned long long>(addr - base));
        return b;
    }
    if (addr >= mem::kHeapBase && addr < mem::kHeapBase + mem::kHeapMax) {
        std::snprintf(b, sizeof b, "heap+0x%llx", static_cast<unsigned long long>(addr - mem::kHeapBase));
        return b;
    }
    if (addr >= mem::kAliasBase && addr < mem::kAliasBase + mem::kAliasSize) {
        std::snprintf(b, sizeof b, "alias+0x%llx", static_cast<unsigned long long>(addr - mem::kAliasBase));
        return b;
    }
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(addr));
    return b;
}

std::vector<Region> regions() {
    std::vector<Region> out;
    const auto mods = modules();
    auto add_range = [&](uint64_t lo, uint64_t hi) {
        for (uint64_t a = lo; a < hi;) {
            const mem::VmInfo v = mem::vm_query(a);
            if (v.size == 0) break;
            const uint64_t next = v.addr + v.size;
            if (next <= a) break;
            if (v.state != mem::kMemFree && v.state != mem::kMemInaccessible) {
                Region r;
                r.addr = std::max(v.addr, lo);
                r.size = std::min(next, hi) - r.addr;
                r.state = v.state;
                r.perm = v.perm;
                for (Module* m : mods)
                    if (m->contains(r.addr)) { r.module = m->name(); break; }
                switch (v.state) {
                    case mem::kMemCode: r.kind = "code"; break;
                    case mem::kMemCodeData: r.kind = "data"; break;
                    case mem::kMemHeap: r.kind = r.addr >= mem::kAliasBase && r.addr < mem::kAliasBase + mem::kAliasSize ? "alias" : "heap"; break;
                    case mem::kMemStack: r.kind = "stack"; break;
                    case mem::kMemThreadLocal: r.kind = "tls"; break;
                    case mem::kMemShared: case mem::kMemTransfer: r.kind = "shared"; break;
                    case mem::kMemAlias: r.kind = "alias"; break;
                    default: r.kind = "other"; break;
                }
                if (r.size) out.push_back(r);
            }
            a = next;
        }
    };
    // the vm map ends at its own limit (memory.cpp kVmEnd); on macOS the alias region lies above it, so it is walked on its own
    const uint64_t lo1 = std::min<uint64_t>(mem::kAliasBase, mem::kMainBase);
    add_range(lo1, 0x8000000000ull);
    if (mem::kAliasBase >= 0x8000000000ull) add_range(mem::kAliasBase, mem::kAliasBase + mem::kAliasSize);
    std::sort(out.begin(), out.end(), [](const Region& a, const Region& b) { return a.addr < b.addr; });
    return out;
}

}  // namespace nxrt::cheats::gmem
