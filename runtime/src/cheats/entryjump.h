// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// A compiled function's entry redirected to other code while the game runs, and put back: how a code cheat reaches the game when the
// game was built without live-patch checks (the default since 2026-11-13: cheats cost nothing while no code cheat is
// on, and only the functions a code cheat patches while one is). pkp_build compiles every lifted function with room before its entry
// (-fpatchable-function-entry with all of it before the entry: 16 bytes of nops on arm64, 14 bytes on x86-64), so the function's own
// instructions are those of a build without the room. A redirect writes a far jump into that room, then one branch over the function's
// first instruction (LLVM never branches to a function's first instruction: its entry block has no predecessors), so every call, from
// any caller, runs the new code; the room's nops fall through to the original entry once the branch is gone.
//   - macOS arm64: the code page is replaced by a copy holding the change (mach_vm_remap over it): every thread sees the old page or the
//     new one, never a mix (measured: 4 threads, 34M calls during a redirect and its removal, every result the old or the new one). The
//     app is signed ad hoc without the hardened runtime; a hardened-runtime build needs allow-unsigned-executable-memory or
//     disable-executable-page-protection, with disable-library-validation (docs/CHEATS.md; never allow-jit alone: such a write kills it).
//   - Windows x86-64: the room is written first (no thread runs it yet), then the 2-byte `jmp short` over the first instruction, with the
//     game's other threads suspended and none of them stopped inside those 2 bytes (as debuggers and Detours change live code).
// A redirect goes through a small stub that records that the new code ran (entered()): a patched function the game never calls again
// (it sits in a loop it never leaves) shows in the cheats list.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nxrt::cheats::ej {

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr size_t kRoom = 16;               // ldr x16, #8 ; br x16 ; .quad target
constexpr size_t kHead = 4;                // b -16
#elif defined(__x86_64__) || defined(_M_X64)
constexpr size_t kRoom = 14;               // jmp [rip+0] ; .quad target
constexpr size_t kHead = 2;                // jmp short -16
#else
constexpr size_t kRoom = 0;
constexpr size_t kHead = 0;
#endif

// The room before the compiled entry is there: the padding's nops (or a jump of ours).
bool has_room(const void* entry);

// What a redirect replaced, to put back.
struct Saved {
    uint8_t room[16] = {};
    uint8_t head[4] = {};
    bool on = false;
};

// The threads to stop while the entry bytes change (Windows: every guest thread but the calling one; their native handles). Unused where
// the change is one page swap (macOS).
void set_threads(std::function<std::vector<void*>()> others);

// Sends every call of `entry` to `target`. *why on failure (the host refused the code change).
bool redirect(void* entry, const void* target, Saved* saved, std::string* why);
// Puts the entry back as it was (the branch first, then the room).
bool restore(void* entry, const Saved& saved, std::string* why);

// A stub that sets its flag when it first runs, then jumps to `target` (an executable page of its own, never written again). Null when the
// host gives no executable memory; then redirect straight to the target.
struct Stub {
    const void* code = nullptr;
    const volatile uint64_t* flag = nullptr;
};
Stub make_stub(const void* target);
void free_stub(const Stub& s);
inline bool entered(const Stub& s) { return s.flag && *s.flag != 0; }

}  // namespace nxrt::cheats::ej
