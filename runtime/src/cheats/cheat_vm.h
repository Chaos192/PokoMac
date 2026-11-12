// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Atmosphère's cheat format and cheat VM, written from the behaviour nxnative's docs/reference/cheats.md describes (read there from
// Atmosphère 1.12.1's dmnt; MIT notes). Our own code: no line of Atmosphère (GPL-2) is used.
//
// File (ParseFile): `[Name]` opens a toggleable cheat in the first free entry 1..0x7F, `{Name}` the master (entry 0); words are exactly
// eight hex digits (a longer run is cut into words); whitespace separates; anything else, a word before the first header, more than 0x100
// words in one cheat, a second master with code, no free entry or a file of 64 KiB or more fails the whole file, as on a console (nothing
// runs). An entry with no words stays free (labels vanish). Names keep their first 0x3F characters.
// toggles.txt (ParseToggles): `[Name]` then 1/true/on or 0/false/off; unknown names and tokens are ignored, a token of 8 or more characters
// fails it.
//
// VM (Vm::run): one program per pass: the words of every enabled entry, master first, then the entries in index order (past 0x400 words
// nothing runs). Registers, saved values, loop tops and the condition depth are reset once per pass and shared by every cheat of the pass.
// Opcodes 0-A, C0-C4, FF0, FF1, FFF. An unknown opcode or a word missing at the end stops the pass there (later cheats do not run). Failed
// reads leave the destination as it was (compares see 0); failed writes are dropped. Where the console's dmnt aborts or hangs we stop the
// pass and report it: an else at depth 0 (dmnt aborts) and a pass of more than 4,000,000 instructions (dmnt's thread would never return).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::cheats {

constexpr size_t kMaxEntries = 0x80;          // master + 127
constexpr size_t kMaxWordsPerCheat = 0x100;
constexpr size_t kMaxProgramWords = 0x400;
constexpr size_t kMaxFileBytes = 0x10000;     // a file of this size or more is refused

struct CheatEntry {
    bool used = false;                        // has words (free entries have none)
    bool enabled = false;
    std::string name;
    std::vector<uint32_t> words;
    int line = 0;                             // line of the header in the file (1-based)
};

struct CheatFile {
    bool ok = false;
    std::string error;                        // why the file was refused, with its line
    std::array<CheatEntry, kMaxEntries> entries;
    size_t count() const;                     // used entries
};

// Parses an Atmosphère cheat file. On failure ok = false and error says where (the console would run nothing).
CheatFile ParseFile(const std::string& text);
// Applies toggles.txt. False with *err when the console would refuse it (dmnt then attaches no cheats at all).
bool ParseToggles(const std::string& text, CheatFile& f, std::string* err);
// The toggles.txt the console writes at detach: `[Name]` and true / false for each non-master cheat.
std::string WriteToggles(const CheatFile& f);
// The file text of the entries (for export and for saving edits), in Atmosphère's layout.
std::string WriteFile(const CheatFile& f);

// What the VM touches. The engine implements it on the guest's memory; the tests on a byte array.
class VmHost {
public:
    virtual ~VmHost() = default;
    virtual bool read(uint64_t addr, void* dst, size_t n) = 0;
    virtual bool write(uint64_t addr, const void* src, size_t n) = 0;
    virtual uint64_t base(unsigned mem_type) = 0;          // 0 main, 1 heap, 2 alias, 3 aslr, 4 absolute (0), 5..F main
    virtual uint64_t keys_held() = 0;                      // HidNpadButton bits held on any controller
    virtual void debug_log(uint32_t id, uint64_t value) { (void)id; (void)value; }   // FFF
    virtual void pause_game(bool pause) { (void)pause; }    // FF0 (true) / FF1 (false)
};

struct PassReport {
    bool ran = false;                 // a program was loaded and executed
    bool stopped = false;             // the pass ended before its last word
    std::string why;                  // why it stopped: "unknown opcode 0xB0000000 in [Name]", "else at depth 0 ...", "over 0x400 words"
    uint64_t steps = 0;
    uint32_t failed_reads = 0, failed_writes = 0;
    uint32_t writes = 0;
};

class Vm {
public:
    // Builds the pass program from the enabled entries; false when they exceed 0x400 words (then nothing runs, as on a console).
    bool load(const CheatFile& f);
    PassReport run(VmHost& host);
    void reset_static();              // static registers to 0 (a new attach)
    uint64_t static_reg(size_t i) const { return i < 0x100 ? static_[i] : 0; }
    void set_static_reg(size_t i, uint64_t v) { if (i < 0x100) static_[i] = v; }
    // The entry whose words hold program word `ip` (for reports).
    int entry_at(size_t ip) const;
    size_t program_words() const { return prog_.size(); }
    bool loaded() const { return loaded_; }
    // Called for each successful VM write (the engine keeps frozen values in step with it, as dmnt does).
    void (*on_write)(uint64_t addr, const void* src, size_t n, void* user) = nullptr;
    void* on_write_user = nullptr;
    uint64_t max_steps = 4000000;

    // One decoded instruction of the pass program (the program is decoded once, front to back, at load: every place execution can reach
    // is an instruction start, since it runs from word 0 and jumps only to loop tops).
    struct Ins {
        uint16_t kind = 0;            // 0x0..0xA, 0xC0..0xC4, 0xFF0, 0xFF1, 0xFFF
        uint8_t w = 0;                // access width T
        uint8_t a = 0, b = 0, c = 0, d = 0, e = 0;   // the nybble fields, by opcode (see cheat_vm.cpp)
        uint64_t imm = 0;             // address offset
        uint64_t val = 0;             // value or mask
        uint32_t word0 = 0;           // the first word (reports)
        size_t at = 0;                // first word index
        size_t next = 0;              // word index after it
    };

private:
    uint64_t mem_read(VmHost& host, uint64_t addr, unsigned width, PassReport& rep);   // zero-extended; 0 on failure
    bool mem_write(VmHost& host, uint64_t addr, uint64_t value, unsigned width, PassReport& rep);
    uint64_t c0_operand(VmHost& host, const Ins& in, PassReport& rep);                 // the comparand of opcode C0
    size_t ins_index_of_word(size_t word) const;   // instruction starting at that word, or ins_.size()
    size_t skip_conditional(size_t from, bool& entered_else) const;                    // an unmet conditional: resume index
    size_t skip_to_end(size_t from) const;                                             // an else reached while executing: resume after its end

    std::vector<uint32_t> prog_;
    std::vector<Ins> ins_;
    size_t valid_ = 0;                            // instructions [0, valid_) decoded; ins_[valid_] (if any) does not
    std::string invalid_why_;                     // why decoding stopped at valid_ (empty: the program ended cleanly)
    std::vector<std::pair<size_t, int>> spans_;   // (first word, entry index)
    std::vector<std::string> names_;              // entry names by index
    bool loaded_ = false;
    uint64_t reg_[16] = {};
    uint64_t saved_[16] = {};
    size_t loop_top_[16] = {};                    // word indexes
    uint64_t static_[0x100] = {};
    uint64_t keyold_ = 0;
};

}  // namespace nxrt::cheats
