// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// [nvn] shader_key_check: a debug diagnostic. SetShaders hashes each stage's code block once (Program::code_hash). If the game fills or moves
// that code after SetShaders (a GPU copy, a pool rewrite), the program keeps a stale shader without any sign. With the key on, the replay
// thread re-hashes the blocks a program points at when it is first drawn (and every Nth draw with a value above 1), compares with the key
// taken at SetShaders, and logs one line per program on a mismatch. The keying itself is not changed. Idea from pokopia-native's lazy shader
// key (host/nxnative-switch2.patch, hle_nvn.c), which found "4 of 6 keys hashed at SetShaders differed from the code at the first draw".
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace nxrt::nvn {

struct KeyStage {
    const uint8_t* code = nullptr;      // host pointers SetShaders was given (null: stage not linked)
    const uint8_t* control = nullptr;
    uint64_t key = 0;                   // the hash taken at SetShaders
};

// What the replay copies at BindProgram, by value: nothing here points into the Program object, which may be finalized before it is drawn.
struct KeySnapshot {
    uintptr_t id = 0;                   // program identity (object address ^ serial)
    KeyStage st[6];
};

// Re-hash of one block (the 16-hex-digit key as a number). Returns false when the control section cannot be trusted.
using RehashFn = bool (*)(const uint8_t* code, const uint8_t* control, uint64_t* key);
bool rehash_block(const uint8_t* code, const uint8_t* control, uint64_t* key);   // the real one: shader_hash over control.code_size

class KeyChecker {
public:
    // every: 1 = first draw of each program, N > 1 = first draw and every Nth draw after it.
    explicit KeyChecker(uint32_t every, RehashFn fn = rehash_block) : every_(every ? every : 1), fn_(fn) {}

    // Count one draw of the program; re-hash when due. Returns the number of stages whose key differs. A program's mismatch is logged
    // once (its first); later mismatches only count. `line` receives the log text when one is due (empty otherwise).
    int on_draw(const KeySnapshot& s, uint64_t frame, std::string* line);

    uint64_t programs_checked() const { return programs_; }
    uint64_t programs_mismatched() const { return bad_programs_; }
    uint64_t stages_mismatched() const { return bad_stages_; }
    uint64_t checks() const { return checks_; }

private:
    struct Entry { uint32_t draws = 0; bool logged = false; };
    uint32_t every_;
    RehashFn fn_;
    std::unordered_map<uintptr_t, Entry> seen_;
    uint64_t programs_ = 0, bad_programs_ = 0, bad_stages_ = 0, checks_ = 0;
};

// Replay-thread entry points. Both are one branch on a cached bool when the key is off.
bool key_check_on();
void key_check_bind(uintptr_t program_id, const KeyStage stages[6]);   // a program was bound (replay thread)
void key_check_draw(uint64_t frame);                                   // a draw begins; checks the last bound program (frame: presents so far)

}  // namespace nxrt::nvn
