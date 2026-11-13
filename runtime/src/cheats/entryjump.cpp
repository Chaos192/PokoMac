// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Entry redirects (entryjump.h).
#include "entryjump.h"

#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#endif

namespace nxrt::cheats::ej {
namespace {

std::mutex g_mu;                                         // one code change at a time
std::function<std::vector<void*>()> g_others;

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr uint32_t kNop = 0xd503201f;
void room_bytes(const void* target, uint8_t* out) {      // ldr x16, #8 ; br x16 ; .quad target
    const uint32_t ldr = 0x58000050, br = 0xd61f0200;
    const uint64_t t = reinterpret_cast<uint64_t>(target);
    std::memcpy(out, &ldr, 4);
    std::memcpy(out + 4, &br, 4);
    std::memcpy(out + 8, &t, 8);
}
void head_bytes(uint8_t* out) {                          // b -16: into the room
    const uint32_t b = 0x17fffffc;
    std::memcpy(out, &b, 4);
}
bool is_room(const uint8_t* p) {
    uint32_t w[4];
    std::memcpy(w, p, 16);
    if (w[0] == kNop && w[1] == kNop && w[2] == kNop && w[3] == kNop) return true;
    return w[0] == 0x58000050 && w[1] == 0xd61f0200;     // our jump from an earlier redirect
}
#elif defined(__x86_64__) || defined(_M_X64)
void room_bytes(const void* target, uint8_t* out) {      // jmp [rip+0] ; .quad target
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const uint64_t t = reinterpret_cast<uint64_t>(target);
    std::memcpy(out, jmp, 6);
    std::memcpy(out + 6, &t, 8);
}
void head_bytes(uint8_t* out) {                          // jmp short -16: entry + 2 - 16 = the room
    out[0] = 0xEB;
    out[1] = 0xF0;
}
bool is_room(const uint8_t* p) {
    bool nops = true;
    for (size_t i = 0; i < 14; ++i) nops = nops && p[i] == 0x90;
    return nops || (p[0] == 0xFF && p[1] == 0x25 && p[2] == 0 && p[3] == 0 && p[4] == 0 && p[5] == 0);
}
#else
void room_bytes(const void*, uint8_t*) {}
void head_bytes(uint8_t*) {}
bool is_room(const uint8_t*) { return false; }
#endif

#if defined(__APPLE__)
// The code page replaced by a copy with the change, in one swap (mach_vm_remap over it): no thread ever runs a page without execute
// permission or half of a change.
bool write_code(uint8_t* at, const uint8_t* bytes, size_t n, std::string* why) {
    const uint64_t ps = vm_page_size, a = reinterpret_cast<uint64_t>(at);
    const uint64_t lo = a & ~(ps - 1), hi = (a + n + ps - 1) & ~(ps - 1), len = hi - lo;
    void* tmp = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (tmp == MAP_FAILED) {
        *why = "no memory for the changed code page";
        return false;
    }
    std::memcpy(tmp, reinterpret_cast<const void*>(lo), len);
    std::memcpy(static_cast<uint8_t*>(tmp) + (a - lo), bytes, n);
    kern_return_t kr = KERN_FAILURE;
    mach_vm_address_t dst = lo;
    vm_prot_t cur = 0, mx = 0;
    if (mprotect(tmp, len, PROT_READ | PROT_EXEC) == 0) {
        sys_icache_invalidate(tmp, len);
        kr = mach_vm_remap(mach_task_self(), &dst, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                           reinterpret_cast<mach_vm_address_t>(tmp), FALSE, &cur, &mx, VM_INHERIT_COPY);
    }
    munmap(tmp, len);
    if (kr != KERN_SUCCESS || dst != lo) {
        *why = "this Mac did not let the program's code page be replaced (mach_vm_remap " + std::to_string(kr) +
               "; a hardened-runtime build needs allow-unsigned-executable-memory)";
        return false;
    }
    sys_icache_invalidate(at, n);
    return true;
}
#elif defined(_WIN32)
// The game's other threads stopped, none of them inside [lo, hi), the bytes written, the threads let go (debuggers and Detours change
// live code the same way). False when a thread kept stopping inside the range (retried for about a second).
bool write_code(uint8_t* at, const uint8_t* bytes, size_t n, std::string* why) {
    const uint64_t lo = reinterpret_cast<uint64_t>(at), hi = lo + n;
    std::vector<void*> threads = g_others ? g_others() : std::vector<void*>{};
    std::vector<HANDLE> stopped;
    stopped.reserve(threads.size());
    for (int attempt = 0; attempt < 1000; ++attempt) {
        stopped.clear();
        bool inside = false;
        for (void* t : threads) {
            HANDLE h = static_cast<HANDLE>(t);
            if (!h || SuspendThread(h) == static_cast<DWORD>(-1)) continue;
            stopped.push_back(h);
            CONTEXT ctx;
            std::memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            // stopped at an instruction boundary: at lo it runs the new bytes from their start; inside, it would resume mid-instruction
            if (GetThreadContext(h, &ctx) && ctx.Rip > lo && ctx.Rip < hi) inside = true;
        }
        if (!inside) {
            DWORD old = 0, tmp = 0;
            bool ok = VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old) != 0;
            if (ok) {
                if (n == 2) {
                    uint16_t v;
                    std::memcpy(&v, bytes, 2);
#if defined(__clang__) || defined(__GNUC__)
                    __atomic_store_n(reinterpret_cast<uint16_t*>(at), v, __ATOMIC_SEQ_CST);   // one 2-byte store: the jump appears whole
#else
                    _InterlockedExchange16(reinterpret_cast<volatile short*>(at), static_cast<short>(v));
#endif
                } else {
                    std::memcpy(at, bytes, n);
                }
                VirtualProtect(at, n, old, &tmp);
                FlushInstructionCache(GetCurrentProcess(), at, n);
            }
            for (HANDLE h : stopped) ResumeThread(h);
            if (!ok) *why = "this host did not let the program's code be changed (VirtualProtect " + std::to_string(GetLastError()) + ")";
            return ok;
        }
        for (HANDLE h : stopped) ResumeThread(h);
        Sleep(1);
    }
    *why = "a game thread kept stopping inside the code to change";
    return false;
}
#else
// Linux: the page made writable for the copy and put back (no other host here changes running code; the game's other threads are held
// at the frame boundary by the caller).
bool write_code(uint8_t* at, const uint8_t* bytes, size_t n, std::string* why) {
    const uint64_t ps = static_cast<uint64_t>(sysconf(_SC_PAGESIZE)), a = reinterpret_cast<uint64_t>(at);
    const uint64_t lo = a & ~(ps - 1), hi = (a + n + ps - 1) & ~(ps - 1);
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        *why = "this host did not let the program's code be changed (mprotect)";
        return false;
    }
    std::memcpy(at, bytes, n);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char*>(at), reinterpret_cast<char*>(at) + n);
    return true;
}
#endif

}  // namespace

void set_threads(std::function<std::vector<void*>()> others) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_others = std::move(others);
}

bool has_room(const void* entry) {
    if (!kRoom || !entry) return false;
    return is_room(static_cast<const uint8_t*>(entry) - kRoom);
}

bool redirect(void* entry, const void* target, Saved* saved, std::string* why) {
    if (!kRoom) {
        *why = "no entry redirect on this host";
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    uint8_t* e = static_cast<uint8_t*>(entry);
    if (!is_room(e - kRoom)) {
        *why = "the compiled function has no room before its entry (a build from before entry room)";
        return false;
    }
    uint8_t room[16], head[4];
    room_bytes(target, room);
    head_bytes(head);
    if (!saved->on) {
        std::memcpy(saved->room, e - kRoom, kRoom);
        std::memcpy(saved->head, e, kHead);
    }
    // an entry redirected already: its branch goes first, so no thread runs the room while it changes
    if (saved->on && !write_code(e, saved->head, kHead, why)) return false;
    if (!write_code(e - kRoom, room, kRoom, why)) return false;
    if (!write_code(e, head, kHead, why)) {
        std::string w2;
        write_code(e - kRoom, saved->room, kRoom, &w2);
        return false;
    }
    saved->on = true;
    return true;
}

bool restore(void* entry, const Saved& saved, std::string* why) {
    if (!saved.on) return true;
    std::lock_guard<std::mutex> lk(g_mu);
    uint8_t* e = static_cast<uint8_t*>(entry);
    if (!write_code(e, saved.head, kHead, why)) return false;   // calls take the function's own first instruction again
    return write_code(e - kRoom, saved.room, kRoom, why);      // the room's nops fall through to it anyway
}

Stub make_stub(const void* target) {
    Stub s;
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)
    // ldr x16, =flag ; ldr x17, [x16] ; cbnz x17, 1f ; mov x17, #1 ; str x17, [x16] ; 1: ldr x17, =target ; br x17 ; nop ; .quad target ;
    // .quad flag   (x16 and x17 are the intra-procedure-call scratch registers: free at a function's entry)
    const size_t ps = static_cast<size_t>(getpagesize());
    void* data = mmap(nullptr, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void* code = mmap(nullptr, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (data == MAP_FAILED || code == MAP_FAILED) {
        if (data != MAP_FAILED) munmap(data, ps);
        if (code != MAP_FAILED) munmap(code, ps);
        return s;
    }
    const uint32_t w[8] = {0x58000150, 0xf9400211, 0xb5000071, 0xd2800031, 0xf9000211, 0x58000071, 0xd61f0220, 0xd503201f};
    const uint64_t lit[2] = {reinterpret_cast<uint64_t>(target), reinterpret_cast<uint64_t>(data)};
    std::memcpy(code, w, sizeof w);
    std::memcpy(static_cast<uint8_t*>(code) + 32, lit, sizeof lit);
    if (mprotect(code, ps, PROT_READ | PROT_EXEC) != 0) {
        munmap(data, ps);
        munmap(code, ps);
        return s;
    }
#if defined(__APPLE__)
    sys_icache_invalidate(code, 48);
#else
    __builtin___clear_cache(static_cast<char*>(code), static_cast<char*>(code) + 48);
#endif
    s.code = code;
    s.flag = static_cast<const volatile uint64_t*>(data);
#elif defined(__x86_64__) || defined(_M_X64)
    // mov r11, flag ; cmp byte [r11], 0 ; jne 1f ; mov byte [r11], 1 ; 1: jmp [rip+0] ; .quad target   (r11: volatile, no argument)
    uint8_t* code = nullptr;
    uint8_t* data = nullptr;
#if defined(_WIN32)
    code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    data = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
    void* c = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void* d = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    code = c == MAP_FAILED ? nullptr : static_cast<uint8_t*>(c);
    data = d == MAP_FAILED ? nullptr : static_cast<uint8_t*>(d);
#endif
    if (!code || !data) return s;
    uint8_t* p = code;
    const uint64_t fl = reinterpret_cast<uint64_t>(data), t = reinterpret_cast<uint64_t>(target);
    *p++ = 0x49; *p++ = 0xBB; std::memcpy(p, &fl, 8); p += 8;
    const uint8_t rest[] = {0x41, 0x80, 0x3B, 0x00, 0x75, 0x04, 0x41, 0xC6, 0x03, 0x01, 0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(p, rest, sizeof rest); p += sizeof rest;
    std::memcpy(p, &t, 8);
#if defined(_WIN32)
    DWORD old = 0;
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old)) return s;
    FlushInstructionCache(GetCurrentProcess(), code, 64);
#else
    if (mprotect(code, 4096, PROT_READ | PROT_EXEC) != 0) return s;
#endif
    s.code = code;
    s.flag = reinterpret_cast<const volatile uint64_t*>(data);
#else
    (void)target;
#endif
    return s;
}

void free_stub(const Stub& s) {
    if (!s.code) return;
#if defined(_WIN32)
    VirtualFree(const_cast<void*>(s.code), 0, MEM_RELEASE);
    VirtualFree(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(s.flag)), 0, MEM_RELEASE);
#else
    const size_t ps = static_cast<size_t>(getpagesize());
    munmap(const_cast<void*>(s.code), ps);
    munmap(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(s.flag)), ps);
#endif
}

}  // namespace nxrt::cheats::ej
