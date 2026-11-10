// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: virtual memory and sections over mach_vm.
//
// Reservations (VirtualAlloc MEM_RESERVE) are mach_vm allocations at the exact address (VM_FLAGS_FIXED fails instead of replacing what is
// there, like Windows) recorded in a region table; commit and protection changes are mprotect on whole host pages. Decommit replaces the
// pages with fresh zero pages (so a later commit reads zeros, as on Windows) and makes them inaccessible.
// A section (CreateFileMapping on the pagefile) is a Mach named memory entry; each view maps it with mach_vm_map (copy = FALSE: every
// view shares the same pages). SEC_RESERVE is kept per host page of the section: committing through any view makes the pages accessible
// in every view of them, as committing section pages does on Windows; VirtualProtect stays per view.
// VirtualProtect and VirtualQuery use no locks, because the write tracker calls them from its fault handler.
#include "nxwin_internal.h"

#include <psapi.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <map>

namespace nxwin {

size_t host_page() { return vm_page_size; }

namespace {
constexpr uint64_t kGran = 0x10000;   // Windows allocation granularity

uint64_t page_down(uint64_t v) { return v & ~(static_cast<uint64_t>(vm_page_size) - 1); }
uint64_t page_up(uint64_t v) { return (v + vm_page_size - 1) & ~(static_cast<uint64_t>(vm_page_size) - 1); }

// Execute access is never given: the only memory that asks for it is the guest's code (mem::Prot::RX / RWX), which the host never runs
// (it is recompiled) and only reads. Asking for PROT_EXEC on pages whose maximum protection lacks it fails with EACCES, which left the
// module images writable ("protect …: VirtualProtect failed, error 5" at every boot).
int prot_of(DWORD p) {
    switch (p & 0xFF) {
        case PAGE_NOACCESS: return PROT_NONE;
        case PAGE_READONLY: return PROT_READ;
        case PAGE_READWRITE: case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
        case PAGE_EXECUTE: return PROT_READ;
        case PAGE_EXECUTE_READ: return PROT_READ;
        case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE;
        default: return PROT_READ | PROT_WRITE;
    }
}
DWORD win_prot(vm_prot_t p) {
    const bool r = p & VM_PROT_READ, w = p & VM_PROT_WRITE, x = p & VM_PROT_EXECUTE;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

struct SectionObj;
struct Region {
    uint64_t size = 0;
    SectionObj* sec = nullptr;   // a view of this section, or a private reservation
    uint64_t offset = 0;         // section offset of the view's first byte
    DWORD alloc_prot = PAGE_NOACCESS;
};
std::mutex g_m;                  // guards g_regions and every section's state
std::map<uint64_t, Region> g_regions;

struct SectionObj final : Object {
    mach_port_t entry = MACH_PORT_NULL;
    uint64_t size = 0;
    bool reserve = false;
    std::vector<uint64_t> committed;   // SEC_RESERVE: one bit per host page of the section
    ~SectionObj() override {
        if (entry != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), entry);
    }
    bool bit(uint64_t p) const { return committed[p >> 6] >> (p & 63) & 1; }
    void set(uint64_t p) { committed[p >> 6] |= 1ull << (p & 63); }
};

// The region (reservation or view) that holds addr, with g_m held; g_regions.end() if none.
std::map<uint64_t, Region>::iterator region_at(uint64_t addr) {
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return g_regions.end();
    --it;
    return addr < it->first + it->second.size ? it : g_regions.end();
}

// ---- holds: plain arrays (vm_hold runs from an early constructor, before any C++ static initialisation of this file) ----
constexpr int kMaxHolds = 256;
uint64_t g_hold_lo[kMaxHolds], g_hold_hi[kMaxHolds];
int g_holds = 0;

// [a, a + size) lies wholly inside held address space (the pieces vm_hold took, which may be adjacent).
bool in_hold(uint64_t a, uint64_t size) {
    uint64_t cur = a;
    const uint64_t end = a + size;
    for (bool moved = true; moved && cur < end;) {
        moved = false;
        for (int i = 0; i < g_holds; ++i)
            if (g_hold_lo[i] <= cur && cur < g_hold_hi[i]) {
                cur = g_hold_hi[i];
                moved = true;
            }
    }
    return cur >= end;
}
// Gives [a, a + size) (inside held space) back to the hold: inaccessible again, no memory.
void rehold(uint64_t a, uint64_t size) {
    mach_vm_address_t p = a;
    mach_vm_map(mach_task_self(), &p, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL,
                VM_INHERIT_DEFAULT);
}
// Whether [a, a + size) overlaps a reservation or view this module made (with g_m held).
bool overlaps_region(uint64_t a, uint64_t size) {
    auto it = g_regions.lower_bound(a + size);
    if (it == g_regions.begin()) return false;
    --it;
    return it->first + it->second.size > a;
}

bool fixed_alloc(uint64_t at, uint64_t size, int prot) {
    mach_vm_address_t a = at;
    int flags = VM_FLAGS_FIXED;
    if (in_hold(at, size)) {
        if (overlaps_region(at, size)) return false;   // our own reservation is there: taken, as on Windows
        flags |= VM_FLAGS_OVERWRITE;
    }
    if (mach_vm_allocate(mach_task_self(), &a, size, flags) != KERN_SUCCESS) return false;
    if (prot != (PROT_READ | PROT_WRITE)) mprotect(reinterpret_cast<void*>(a), size, prot);
    return true;
}
void release_range(uint64_t a, uint64_t size) {
    if (in_hold(a, size)) rehold(a, size);
    else mach_vm_deallocate(mach_task_self(), a, size);
}
uint64_t anywhere_alloc(uint64_t size, int prot) {
    mach_vm_address_t a = 0;
    const kern_return_t kr = mach_vm_map(mach_task_self(), &a, size, kGran - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE,
                                         static_cast<vm_prot_t>(prot), VM_PROT_ALL, VM_INHERIT_DEFAULT);
    return kr == KERN_SUCCESS ? a : 0;
}

// Commits [start, end) (host-page aligned) of a section through any of its views, with g_m held.
void section_commit(SectionObj* s, uint64_t off_start, uint64_t off_end, int prot) {
    const uint64_t pg = vm_page_size;
    for (uint64_t o = off_start; o < off_end; o += pg) s->set(o / pg);
    for (auto& [base, r] : g_regions) {
        if (r.sec != s) continue;
        const uint64_t a = std::max(off_start, r.offset), b = std::min(off_end, r.offset + r.size);
        if (a < b) mprotect(reinterpret_cast<void*>(base + (a - r.offset)), b - a, prot);
    }
}
}  // namespace

uint64_t vm_hold(uint64_t addr, uint64_t size) {
    // Walk the range: reserve each gap between existing mappings (inaccessible, no memory), skip what is mapped already.
    uint64_t missed = 0;
    uint64_t cur = addr;
    const uint64_t end = addr + size;
    while (cur < end) {
        mach_vm_address_t r = cur;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        const bool found = mach_vm_region(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &cnt,
                                          &obj) == KERN_SUCCESS;
        const uint64_t gap_end = found ? std::min<uint64_t>(r, end) : end;   // free until the next mapping
        if (gap_end > cur) {
            mach_vm_address_t p = cur;
            if (mach_vm_map(mach_task_self(), &p, gap_end - cur, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL,
                            VM_INHERIT_DEFAULT) == KERN_SUCCESS &&
                g_holds < kMaxHolds) {
                g_hold_lo[g_holds] = cur;
                g_hold_hi[g_holds] = gap_end;
                ++g_holds;
            } else {
                missed += gap_end - cur;
            }
            cur = gap_end;
            continue;
        }
        // cur is inside an existing mapping: skip it
        const uint64_t next = std::min<uint64_t>(r + rs, end);
        missed += next - cur;
        cur = next;
    }
    return missed;
}

bool safe_read(void* dst, const void* src, size_t n) {
    if (!n) return true;
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(src), n, reinterpret_cast<mach_vm_address_t>(dst),
                                  &got) == KERN_SUCCESS && got == n;
}
bool safe_write(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace nxwin

using namespace nxwin;

LPVOID VirtualAlloc(LPVOID addr_in, SIZE_T size_in, DWORD type, DWORD prot) {
    if (!size_in) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    const uint64_t addr = reinterpret_cast<uint64_t>(addr_in);
    const int p = prot_of(prot);
    std::lock_guard<std::mutex> g(g_m);
    if ((type & MEM_RESERVE) || !addr) {
        uint64_t base, size;
        if (addr) {
            base = addr & ~(kGran - 1);
            size = page_up(addr + size_in) - base;
            if (!fixed_alloc(base, size, (type & MEM_COMMIT) ? p : PROT_NONE)) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
        } else {
            size = page_up(size_in);
            base = anywhere_alloc(size, (type & MEM_COMMIT) ? p : PROT_NONE);
            if (!base) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        }
        g_regions[base] = Region{size, nullptr, 0, prot};
        return reinterpret_cast<LPVOID>(addr ? addr : base);
    }
    if (!(type & MEM_COMMIT)) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    auto it = region_at(addr);
    if (it == g_regions.end() || addr + size_in > it->first + it->second.size) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
    const uint64_t s = page_down(addr), e = page_up(addr + size_in);
    if (it->second.sec) {
        Region& r = it->second;
        section_commit(r.sec, r.offset + (s - it->first), r.offset + (e - it->first), p);
    } else if (mprotect(reinterpret_cast<void*>(s), e - s, p) != 0) {
        set_errno_error();
        return nullptr;
    }
    return reinterpret_cast<LPVOID>(addr & ~uint64_t{0xFFF});
}
LPVOID VirtualAllocEx(HANDLE, LPVOID addr, SIZE_T size, DWORD type, DWORD prot) { return VirtualAlloc(addr, size, type, prot); }

BOOL VirtualFree(LPVOID addr_in, SIZE_T size, DWORD type) {
    const uint64_t addr = reinterpret_cast<uint64_t>(addr_in);
    std::lock_guard<std::mutex> g(g_m);
    auto it = region_at(addr);
    if (it == g_regions.end() || it->second.sec) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (type & MEM_RELEASE) {
        if (size || it->first != (addr & ~(kGran - 1))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        release_range(it->first, it->second.size);
        g_regions.erase(it);
        return TRUE;
    }
    if (type & MEM_DECOMMIT) {
        const uint64_t end = size ? addr + size : it->first + it->second.size;
        const uint64_t s = page_up(addr), e = page_down(end);
        if (s < e) {   // whole host pages: fresh zero pages, no access
            if (mmap(reinterpret_cast<void*>(s), e - s, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
                set_errno_error();
                return FALSE;
            }
        }
        // Parts of a host page that the range covers only partly stay committed; zero them so a later commit reads zeros.
        auto zero = [](uint64_t a, uint64_t b) {
            MEMORY_BASIC_INFORMATION mbi;
            if (a < b && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                std::memset(reinterpret_cast<void*>(a), 0, b - a);
        };
        const uint64_t a0 = addr & ~uint64_t{0xFFF}, e0 = (end + 0xFFF) & ~uint64_t{0xFFF};
        if (s >= e) zero(a0, e0);
        else { zero(a0, s); zero(e, e0); }
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL VirtualProtect(LPVOID addr_in, SIZE_T size, DWORD prot, PDWORD old) {
    const uint64_t addr = reinterpret_cast<uint64_t>(addr_in);
    if (old) {
        mach_vm_address_t a = page_down(addr);
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        *old = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) ==
                       KERN_SUCCESS && a <= addr ? win_prot(info.protection) : PAGE_NOACCESS;
    }
    // Host pages wholly inside the range get exactly `prot`. A host page the range covers only partly also holds guest pages (4 KiB) the
    // caller did not name: it gets the union of `prot` and what it has, so no neighbouring page loses access it had (more permissive, never less).
    const uint64_t pg = vm_page_size, a0 = addr, a1 = addr + (size ? size : 1);
    const uint64_t s = page_down(a0), e = page_up(a1), fs = page_up(a0), fe = page_down(a1);
    const int np = prot_of(prot);
    auto fail = [] {
        SetLastError(errno == ENOMEM ? ERROR_INVALID_ADDRESS : errno_to_win32(errno));
        return FALSE;
    };
    if (fs == s && fe == e) {
        if (mprotect(reinterpret_cast<void*>(s), e - s, np) != 0) return fail();
        return TRUE;
    }
    auto cur_prot = [](uint64_t p) {
        mach_vm_address_t a = p;
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) != KERN_SUCCESS ||
            a > p)
            return PROT_NONE;
        return static_cast<int>(info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE));
    };
    if (fs < fe && mprotect(reinterpret_cast<void*>(fs), fe - fs, np) != 0) return fail();
    if (s < fs && mprotect(reinterpret_cast<void*>(s), pg, np | cur_prot(s)) != 0) return fail();
    if (fe < e && fe >= fs && mprotect(reinterpret_cast<void*>(fe), pg, np | cur_prot(fe)) != 0) return fail();
    return TRUE;
}

SIZE_T VirtualQuery(LPCVOID addr_in, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len) {
    if (len < sizeof *mbi) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    const uint64_t addr = page_down(reinterpret_cast<uint64_t>(addr_in));
    std::memset(mbi, 0, sizeof *mbi);
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    const kern_return_t kr =
        mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj);
    mbi->BaseAddress = reinterpret_cast<PVOID>(addr);
    if (kr != KERN_SUCCESS || a > addr) {        // a gap: free up to the next region (or the end of the address space)
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
        mbi->RegionSize = kr == KERN_SUCCESS ? a - addr : 0x800000000000ull - addr;
        return sizeof *mbi;
    }
    mbi->AllocationBase = reinterpret_cast<PVOID>(a);
    mbi->AllocationProtect = win_prot(info.max_protection & VM_PROT_ALL);
    mbi->RegionSize = a + sz - addr;
    mbi->Protect = win_prot(info.protection);
    mbi->State = info.protection == VM_PROT_NONE ? MEM_RESERVE : MEM_COMMIT;
    if (mbi->State == MEM_RESERVE) mbi->Protect = 0;
    mbi->Type = info.shared ? MEM_MAPPED : MEM_PRIVATE;
    return sizeof *mbi;
}

BOOL VirtualLock(LPVOID addr, SIZE_T size) { return mlock(addr, size) == 0; }
BOOL VirtualUnlock(LPVOID addr, SIZE_T size) { return munlock(addr, size) == 0; }
BOOL FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return TRUE; }

// ---- sections and views --------------------------------------------------------------------------------------------------------------------
HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD prot, DWORD size_hi, DWORD size_lo, LPCSTR name) {
    if (file != INVALID_HANDLE_VALUE || name) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }   // pagefile-backed, unnamed only
    memory_object_size_t size = page_up((static_cast<uint64_t>(size_hi) << 32) | size_lo);
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    auto* s = new SectionObj;
    s->reserve = prot & SEC_RESERVE;
    const vm_prot_t vp = VM_PROT_READ | VM_PROT_WRITE | ((prot & 0xF0) ? VM_PROT_EXECUTE : 0);
    const memory_object_size_t want = size;
    if (mach_make_memory_entry_64(mach_task_self(), &size, 0, MAP_MEM_NAMED_CREATE | vp, &s->entry, MACH_PORT_NULL) != KERN_SUCCESS ||
        size < want) {
        s->unref();
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    s->size = size;
    if (s->reserve) s->committed.assign((size / vm_page_size + 63) / 64, 0);
    return handle_of(s);
}
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD size_hi, DWORD size_lo, LPCWSTR name) {
    return CreateFileMappingA(file, sa, prot, size_hi, size_lo, name ? narrow(name).c_str() : nullptr);
}

LPVOID MapViewOfFileEx(HANDLE h, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size_in, LPVOID base) {
    auto* s = as<SectionObj>(h);
    if (!s) { SetLastError(ERROR_INVALID_HANDLE); return nullptr; }
    const uint64_t off = (static_cast<uint64_t>(off_hi) << 32) | off_lo;
    if ((off & (kGran - 1)) || off >= s->size) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    const uint64_t size = page_up(size_in ? size_in : s->size - off);
    if (off + size > s->size) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    vm_prot_t maxp = VM_PROT_READ | ((access & (FILE_MAP_WRITE | FILE_MAP_COPY)) || access == FILE_MAP_ALL_ACCESS ? VM_PROT_WRITE : 0);
    if (access & FILE_MAP_EXECUTE) maxp |= VM_PROT_EXECUTE;
    std::lock_guard<std::mutex> g(g_m);
    mach_vm_address_t a = reinterpret_cast<uint64_t>(base);
    const vm_prot_t cur = s->reserve ? VM_PROT_NONE : maxp;
    int flags = base ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE;
    if (base && in_hold(a, size)) {
        if (overlaps_region(a, size)) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
        flags |= VM_FLAGS_OVERWRITE;
    }
    const kern_return_t kr = mach_vm_map(mach_task_self(), &a, size, base ? 0 : kGran - 1, flags, s->entry, off, FALSE, cur, maxp, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        SetLastError(kr == KERN_NO_SPACE ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    s->ref();                     // a view keeps its section alive
    g_regions[a] = Region{size, s, off, win_prot(maxp)};
    if (s->reserve) {             // pages already committed in the section are accessible in the new view too
        const uint64_t pg = vm_page_size;
        for (uint64_t o = off; o < off + size;) {
            if (!s->bit(o / pg)) { o += pg; continue; }
            uint64_t e = o;
            while (e < off + size && s->bit(e / pg)) e += pg;
            mprotect(reinterpret_cast<void*>(a + (o - off)), e - o, (maxp & VM_PROT_WRITE) ? PROT_READ | PROT_WRITE : PROT_READ);
            o = e;
        }
    }
    return reinterpret_cast<LPVOID>(a);
}
LPVOID MapViewOfFile(HANDLE h, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size) { return MapViewOfFileEx(h, access, off_hi, off_lo, size, nullptr); }
BOOL UnmapViewOfFile(LPCVOID view) {
    SectionObj* s = nullptr;
    {
        std::lock_guard<std::mutex> g(g_m);
        auto it = g_regions.find(reinterpret_cast<uint64_t>(view));
        if (it == g_regions.end() || !it->second.sec) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
        release_range(it->first, it->second.size);
        s = it->second.sec;
        g_regions.erase(it);
    }
    s->unref();
    return TRUE;
}
BOOL FlushViewOfFile(LPCVOID, SIZE_T) { return TRUE; }

BOOL ReadProcessMemory(HANDLE, LPCVOID addr, LPVOID buf, SIZE_T n, SIZE_T* got) {
    const bool ok = safe_read(buf, addr, n);
    if (got) *got = ok ? n : 0;
    if (!ok) SetLastError(ERROR_PARTIAL_COPY);
    return ok;
}
BOOL WriteProcessMemory(HANDLE, LPVOID addr, LPCVOID buf, SIZE_T n, SIZE_T* put) {
    const bool ok = safe_write(addr, buf, n);
    if (put) *put = ok ? n : 0;
    if (!ok) SetLastError(ERROR_NOACCESS);
    return ok;
}
BOOL IsBadReadPtr(const void* p, UINT_PTR n) {
    if (!n) return FALSE;
    if (!p) return TRUE;
    const uint64_t a = reinterpret_cast<uint64_t>(p), end = a + n;
    for (uint64_t q = a; q < end; q = page_down(q) + vm_page_size) {
        char c;
        if (!safe_read(&c, reinterpret_cast<const void*>(q), 1)) return TRUE;
    }
    char c;
    return safe_read(&c, reinterpret_cast<const void*>(end - 1), 1) ? FALSE : TRUE;
}
BOOL IsBadWritePtr(LPVOID p, UINT_PTR n) {
    if (!n) return FALSE;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint64_t q = reinterpret_cast<uint64_t>(p); q < reinterpret_cast<uint64_t>(p) + n; q = page_down(q) + vm_page_size) {
        if (!VirtualQuery(reinterpret_cast<void*>(q), &mbi, sizeof mbi) || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return TRUE;
    }
    return FALSE;
}

void GetSystemInfo(LPSYSTEM_INFO si) {
    std::memset(si, 0, sizeof *si);
#if defined(__aarch64__)
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
#else
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
#endif
    si->dwPageSize = static_cast<DWORD>(vm_page_size);
    si->lpMinimumApplicationAddress = reinterpret_cast<LPVOID>(0x100000000ull);
    si->lpMaximumApplicationAddress = reinterpret_cast<LPVOID>(0x7FFFFFFFFFFFull);
    si->dwNumberOfProcessors = GetActiveProcessorCount(0);
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~DWORD_PTR{0} : (DWORD_PTR{1} << si->dwNumberOfProcessors) - 1;
    si->dwAllocationGranularity = static_cast<DWORD>(kGran);
}
void GetNativeSystemInfo(LPSYSTEM_INFO si) { GetSystemInfo(si); }

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms) {
    uint64_t total = 0;
    size_t len = sizeof total;
    sysctlbyname("hw.memsize", &total, &len, nullptr, 0);
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    uint64_t avail = total / 2;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vs), &cnt) == KERN_SUCCESS)
        avail = (static_cast<uint64_t>(vs.free_count) + vs.inactive_count + vs.purgeable_count + vs.speculative_count) * vm_page_size;
    ms->dwMemoryLoad = total ? static_cast<DWORD>(100 - avail * 100 / total) : 0;
    ms->ullTotalPhys = total;
    ms->ullAvailPhys = avail;
    ms->ullTotalPageFile = total * 2;          // macOS swaps on demand: report twice the RAM as the commit limit
    ms->ullAvailPageFile = avail + total;
    ms->ullTotalVirtual = 0x7FFFFFFF0000ull;
    ms->ullAvailVirtual = 0x7FFFFFFF0000ull;
    ms->ullAvailExtendedVirtual = 0;
    return TRUE;
}
BOOL GetPhysicallyInstalledSystemMemory(PULONGLONG kb) {
    uint64_t total = 0;
    size_t len = sizeof total;
    sysctlbyname("hw.memsize", &total, &len, nullptr, 0);
    *kb = total / 1024;
    return TRUE;
}

BOOL GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS pmc, DWORD cb) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vi), &cnt) != KERN_SUCCESS) return FALSE;
    std::memset(pmc, 0, std::min<size_t>(cb, sizeof(PROCESS_MEMORY_COUNTERS_EX)));
    pmc->cb = cb;
    pmc->WorkingSetSize = vi.resident_size;
    pmc->PeakWorkingSetSize = vi.resident_size_peak;
    pmc->PagefileUsage = vi.phys_footprint;
    pmc->PeakPagefileUsage = vi.ledger_phys_footprint_peak ? vi.ledger_phys_footprint_peak : vi.phys_footprint;
    if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) reinterpret_cast<PPROCESS_MEMORY_COUNTERS_EX>(pmc)->PrivateUsage = vi.phys_footprint;
    return TRUE;
}
BOOL GetPerformanceInfo(PPERFORMANCE_INFORMATION pi, DWORD cb) {
    std::memset(pi, 0, cb);
    MEMORYSTATUSEX ms{};
    GlobalMemoryStatusEx(&ms);
    pi->cb = cb;
    pi->PageSize = vm_page_size;
    pi->PhysicalTotal = ms.ullTotalPhys / vm_page_size;
    pi->PhysicalAvailable = ms.ullAvailPhys / vm_page_size;
    pi->CommitLimit = ms.ullTotalPageFile / vm_page_size;
    pi->CommitTotal = (ms.ullTotalPhys - ms.ullAvailPhys) / vm_page_size;
    return TRUE;
}
