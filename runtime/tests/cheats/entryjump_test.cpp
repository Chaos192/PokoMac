// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Entry redirects (runtime/src/cheats/entryjump.*) on this host, in this signed program: a function compiled with room before its entry
// (as pkp_build compiles lifted code) is sent to another function while four threads call it, through the stub that records the first
// run, then put back; again straight to the target; every call returns the old or the new result, never anything else, and the process
// lives on. Then the inlined copy: a caller the compiler inlined the function into keeps the old code after the function's redirect; the
// compiled code shows it (inliners.cpp finds no call to the function in it), and redirecting that caller too (to its own code compiled
// again, which calls the function) makes every call run the new one, as the code patcher does. Standalone: includes entryjump.cpp and
// inliners.cpp.
#include "entryjump.h"
#include "inliners.h"

#include "entryjump.cpp"
#include "inliners.cpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace nxrt::cheats;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

#if defined(__aarch64__) || defined(_M_ARM64)
#define ROOM __attribute__((noinline, patchable_function_entry(4, 4)))
#else
#define ROOM __attribute__((noinline, patchable_function_entry(14, 14)))
#endif

extern "C" ROOM int ej_f(int x) { return x + 1; }
extern "C" __attribute__((noinline)) int ej_g(int x) { return x + 1000; }

// the inlined copy: ej_leaf has room and an out-of-line copy of its own (its address is taken), and ej_user has it inlined
#if defined(__aarch64__) || defined(_M_ARM64)
#define ROOM_INLINE __attribute__((always_inline, patchable_function_entry(4, 4)))
#else
#define ROOM_INLINE __attribute__((always_inline, patchable_function_entry(14, 14)))
#endif
extern "C" ROOM_INLINE int ej_leaf(int x) { return x * 3; }   // a strong definition (an inline one is weak: the linker may move it away from its room)
extern "C" ROOM int ej_user(int x) { return ej_leaf(x) + 1; }               // inlined: no call to ej_leaf in its code
extern "C" __attribute__((noinline)) int ej_leaf_new(int x) { return x * 3 + 100; }         // the leaf compiled again from patched code
static int (*volatile g_leaf)(int) = ej_leaf;
static int (*volatile g_user)(int) = ej_user;
extern "C" __attribute__((noinline)) int ej_user_new(int x) { return g_leaf(x) + 1; }       // the user compiled again: it calls the leaf
extern "C" ROOM int ej_caller(int x) { return g_leaf(x) + 1; }                               // keeps a call to the leaf's entry

// The code of a compiled function up to its first return (enough for these small ones).
static size_t code_len(const void* fn) {
    const uint8_t* p = static_cast<const uint8_t*>(fn);
    for (size_t i = 0; i < 4096; i += ej::kHead == 4 ? 4 : 1) {
        if (ej::kHead == 4) {
            uint32_t w;
            std::memcpy(&w, p + i, 4);
            if (w == 0xd65f03c0) return i + 4;
        } else if (p[i] == 0xC3) {
            return i + 1;
        }
    }
    return 4096;
}

static int (*volatile g_call)(int) = ej_f;      // through a pointer: no caller inlines it
static std::atomic<bool> g_stop{false};
static std::atomic<long> g_old{0}, g_new{0}, g_wrong{0};

static void caller() {
    while (!g_stop.load(std::memory_order_relaxed)) {
        const int r = g_call(1);
        if (r == 2) g_old.fetch_add(1, std::memory_order_relaxed);
        else if (r == 1001) g_new.fetch_add(1, std::memory_order_relaxed);
        else g_wrong.fetch_add(1, std::memory_order_relaxed);
    }
}

static bool settles_to(int want) {
    for (int i = 0; i < 2000; ++i) {
        if (g_call(1) == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

int main() {
    if (!ej::kRoom) {
        std::printf("no entry redirect on this host: skipped\n");
        return 0;
    }
    CHECK(ej::has_room(reinterpret_cast<const void*>(ej_f)));
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back(caller);
#if defined(_WIN32)
    std::vector<void*> handles;
    for (auto& t : threads) handles.push_back(t.native_handle());
    ej::set_threads([&] { return handles; });
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    void* entry = reinterpret_cast<void*>(ej_f);
    for (int round = 0; round < 3; ++round) {
        // through the stub: the first call sets its flag
        const ej::Stub stub = ej::make_stub(reinterpret_cast<const void*>(ej_g));
        CHECK(stub.code != nullptr && !ej::entered(stub));
        ej::Saved saved;
        std::string why;
        CHECK(ej::redirect(entry, stub.code ? stub.code : reinterpret_cast<const void*>(ej_g), &saved, &why));
        if (!why.empty()) std::printf("redirect: %s\n", why.c_str());
        CHECK(settles_to(1001));
        CHECK(ej::entered(stub));
        CHECK(ej::has_room(entry));                         // the room holds our jump: a later redirect may rewrite it
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(ej::restore(entry, saved, &why));
        CHECK(settles_to(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // straight to the target, then back
        ej::Saved s2;
        CHECK(ej::redirect(entry, reinterpret_cast<const void*>(ej_g), &s2, &why));
        CHECK(settles_to(1001));
        // a second redirect of an entry already redirected (a cheat's function compiled again): the new target takes over
        CHECK(ej::redirect(entry, stub.code, &s2, &why));
        CHECK(settles_to(1001));
        CHECK(ej::restore(entry, s2, &why));
        CHECK(settles_to(2));
        ej::free_stub(stub);
    }
    // the inlined copy (Apple's linker, at -O2, laid these two out without the nops before them: zig's link of the lifted code keeps the
    // room before every function, 2,772 of 2,772 in Pokopia's subsdk0 and rtld; the runtime reads it, and a function without it keeps the
    // old ways)
    if (!ej::has_room(reinterpret_cast<const void*>(&ej_leaf)) || !ej::has_room(reinterpret_cast<const void*>(&ej_user))) {
        std::printf("the inlined copy: skipped (this link left no room before ej_leaf / ej_user)\n");
    } else {
#if defined(__aarch64__) || defined(_M_ARM64)
        const inl::Host arch = inl::Host::Arm64;
#else
        const inl::Host arch = inl::Host::X64;
#endif
        const uint64_t leaf = reinterpret_cast<uint64_t>(&ej_leaf), user = reinterpret_cast<uint64_t>(&ej_user);
        const bool user_calls = inl::calls(reinterpret_cast<const uint8_t*>(user), code_len(reinterpret_cast<const void*>(&ej_user)), user, leaf, arch);
        std::printf("ej_user %s ej_leaf\n", user_calls ? "calls" : "inlined");
        CHECK(!user_calls);                                 // inlined: a redirect of ej_leaf alone does not reach it
        CHECK(g_leaf(2) == 6 && g_user(2) == 7);
        ej::Saved sl, su;
        std::string why;
        CHECK(ej::redirect(reinterpret_cast<void*>(&ej_leaf), reinterpret_cast<const void*>(&ej_leaf_new), &sl, &why));
        CHECK(g_leaf(2) == 106);
        CHECK(g_user(2) == 7);                              // still the old leaf, inlined
        CHECK(ej::redirect(reinterpret_cast<void*>(&ej_user), reinterpret_cast<const void*>(&ej_user_new), &su, &why));
        CHECK(g_user(2) == 107);                            // its new code calls the leaf: the patched one
        CHECK(ej_caller(2) == 107);                         // a caller that kept its call needs no redirect of its own
        CHECK(ej::restore(reinterpret_cast<void*>(&ej_user), su, &why) && ej::restore(reinterpret_cast<void*>(&ej_leaf), sl, &why));
        CHECK(g_leaf(2) == 6 && g_user(2) == 7 && ej_caller(2) == 7);
    }
    g_stop = true;
    for (auto& t : threads) t.join();
    std::printf("threads: %ld calls returned the old result, %ld the new one, %ld anything else\n", g_old.load(), g_new.load(), g_wrong.load());
    CHECK(g_wrong.load() == 0 && g_old.load() > 0 && g_new.load() > 0);
    CHECK(ej_f(1) == 2);
    if (g_fail == 0) std::printf("ALL ENTRYJUMP TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
