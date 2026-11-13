// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Atmosphère cheat format and VM (cheat_vm.h). Written from nxnative docs/reference/cheats.md; our own code.
#include "cheat_vm.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

namespace nxrt::cheats {
namespace {

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int line_of(const std::string& text, size_t pos) {
    int n = 1;
    for (size_t i = 0; i < pos && i < text.size(); ++i)
        if (text[i] == '\n') ++n;
    return n;
}

}  // namespace

size_t CheatFile::count() const {
    size_t n = 0;
    for (const auto& e : entries)
        if (e.used) ++n;
    return n;
}

// first_free: the lowest free entry 1..0x7F (an entry with no words is free, so a label's slot is reused).
static int first_free(const CheatFile& f) {
    for (size_t i = 1; i < kMaxEntries; ++i)
        if (!f.entries[i].used) return static_cast<int>(i);
    return -1;
}

CheatFile ParseFile(const std::string& text) {
    CheatFile f;
    if (text.size() >= kMaxFileBytes) {
        f.error = "the cheat file is 64 KiB or larger; a console refuses it";
        return f;
    }
    int cur = -1;                         // entry being filled, -1 = none open
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const char c = text[i];
        if (is_ws(c)) { ++i; continue; }
        if (c == '[' || c == '{') {
            const char close = c == '[' ? ']' : '}';
            const size_t start = i + 1;
            size_t j = start;
            while (j < n && text[j] != close && text[j] != '\n') ++j;
            if (j >= n || text[j] != close) {
                f.error = "line " + std::to_string(line_of(text, i)) + ": a cheat name has no closing " + std::string(1, close);
                return f;
            }
            std::string name = text.substr(start, j - start);
            if (name.size() > 0x3F) name.resize(0x3F);
            int slot;
            if (c == '{') {
                if (f.entries[0].used) {
                    f.error = "line " + std::to_string(line_of(text, i)) + ": a second master cheat {...}; a console refuses the file";
                    return f;
                }
                slot = 0;
            } else {
                slot = first_free(f);      // reuses the previous header's slot when it got no words (a label)
                if (slot < 0) {
                    // find a still-empty open entry to reuse, else out of entries
                    slot = (cur >= 0 && !f.entries[cur].used) ? cur : -1;
                    if (slot < 0) {
                        f.error = "line " + std::to_string(line_of(text, i)) + ": more than 127 cheats; a console refuses the file";
                        return f;
                    }
                }
            }
            f.entries[slot].name = name;
            f.entries[slot].line = line_of(text, i);
            f.entries[slot].words.clear();
            f.entries[slot].used = false;   // becomes used when it gets a word
            cur = slot;
            i = j + 1;
            continue;
        }
        if (is_hex(c)) {
            if (cur < 0) {
                f.error = "line " + std::to_string(line_of(text, i)) + ": a code word before any cheat name; a console refuses the file";
                return f;
            }
            // a run of hex digits, cut into 8-digit words wherever it ends
            size_t j = i;
            while (j < n && is_hex(text[j])) ++j;
            const size_t len = j - i;
            if (len % 8 != 0) {
                f.error = "line " + std::to_string(line_of(text, i)) + ": a run of " + std::to_string(len) +
                          " hex digits is not a whole number of 8-digit words; a console refuses the file";
                return f;
            }
            for (size_t k = i; k < j; k += 8) {
                if (f.entries[cur].words.size() >= kMaxWordsPerCheat) {
                    f.error = "line " + std::to_string(f.entries[cur].line) + ": cheat [" + f.entries[cur].name +
                              "] has more than 256 words; a console refuses the file";
                    return f;
                }
                uint32_t w = 0;
                for (size_t m = 0; m < 8; ++m) {
                    const char h = text[k + m];
                    const unsigned d = h <= '9' ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10);
                    w = (w << 4) | d;
                }
                f.entries[cur].words.push_back(w);
                f.entries[cur].used = true;
            }
            i = j;
            continue;
        }
        f.error = "line " + std::to_string(line_of(text, i)) + ": '" + std::string(1, c) +
                  "' is not whitespace, a cheat name or a hex word (there is no comment syntax); a console refuses the file";
        return f;
    }
    f.entries[0].enabled = f.entries[0].used;   // the master is always on
    for (size_t k = 1; k < kMaxEntries; ++k)
        if (f.entries[k].used) f.entries[k].enabled = true;   // default on (dmnt_cheats_enabled_by_default = 1)
    f.ok = true;
    return f;
}

// Cheats that share a name (cheat sites' folder markers, e.g. two "[Movement]" entries around a group) are matched in order: the k-th
// `[Name]` line sets the k-th cheat of that name, so what WriteToggles wrote comes back for each of them (dmnt sets the first one each
// time, which leaves the later ones at the file's default).
bool ParseToggles(const std::string& text, CheatFile& f, std::string* err) {
    size_t i = 0;
    const size_t n = text.size();
    std::string name;
    bool have_name = false;
    std::map<std::string, int> seen;   // name -> how many `[Name]` lines with a value came before
    while (i < n) {
        const char c = text[i];
        if (is_ws(c)) { ++i; continue; }
        if (c == '[') {
            size_t j = i + 1;
            while (j < n && text[j] != ']' && text[j] != '\n') ++j;
            if (j >= n || text[j] != ']') { if (err) *err = "toggles.txt: a name has no closing ]"; return false; }
            name = text.substr(i + 1, j - i - 1);
            have_name = true;
            i = j + 1;
            continue;
        }
        // a token
        size_t j = i;
        while (j < n && !is_ws(text[j])) ++j;
        const std::string tok = text.substr(i, j - i);
        if (tok.size() >= 8) { if (err) *err = "toggles.txt: token '" + tok + "' is 8 or more characters"; return false; }
        if (have_name) {
            const std::string t = lower(tok);
            const bool on = t == "1" || t == "true" || t == "on";
            const bool off = t == "0" || t == "false" || t == "off";
            if (on || off) {
                const int nth = seen[name]++;
                int found = 0;
                CheatEntry* first = nullptr;
                CheatEntry* hit = nullptr;
                for (size_t k = 1; k < kMaxEntries && !hit; ++k) {
                    CheatEntry& e = f.entries[k];
                    if (!e.used || e.name != name) continue;
                    if (!first) first = &e;
                    if (found++ == nth) hit = &e;
                }
                if (!hit) hit = first;   // more lines than cheats of that name: the first one, as before
                if (hit) hit->enabled = on;
            }
            have_name = false;
        }
        i = j;
    }
    return true;
}

std::string WriteToggles(const CheatFile& f) {
    std::string out;
    for (size_t k = 1; k < kMaxEntries; ++k)
        if (f.entries[k].used)
            out += "[" + f.entries[k].name + "]\n" + (f.entries[k].enabled ? "true" : "false") + "\n";
    return out;
}

std::string WriteFile(const CheatFile& f) {
    std::string out;
    char b[16];
    auto emit = [&](const CheatEntry& e, char open, char close) {
        out += open;
        out += e.name;
        out += close;
        out += '\n';
        for (size_t k = 0; k < e.words.size(); ++k) {
            std::snprintf(b, sizeof b, "%08X", e.words[k]);
            out += b;
            out += (k + 1 == e.words.size() || (k % 4) == 3) ? '\n' : ' ';
        }
        out += '\n';
    };
    if (f.entries[0].used) emit(f.entries[0], '{', '}');
    for (size_t k = 1; k < kMaxEntries; ++k)
        if (f.entries[k].used) emit(f.entries[k], '[', ']');
    return out;
}

// ---- the VM ---------------------------------------------------------------------------------------------------------

// Loads the pass program and decodes it once. The program runs from word 0 and only ever jumps to a loop top (a word index saved at a 3-start
// opcode, which is itself an instruction start), so decoding straight through visits every reachable instruction.
bool Vm::load(const CheatFile& f) {
    prog_.clear();
    spans_.clear();
    names_.clear();
    file_entries_.clear();
    ins_.clear();
    valid_ = 0;
    invalid_why_.clear();
    loaded_ = false;
    auto add = [&](const CheatEntry& e, int idx) {
        spans_.emplace_back(prog_.size(), static_cast<int>(names_.size()));
        names_.push_back(e.name);
        file_entries_.push_back(idx);
        prog_.insert(prog_.end(), e.words.begin(), e.words.end());
    };
    if (f.entries[0].used && f.entries[0].enabled) add(f.entries[0], 0);
    for (size_t k = 1; k < kMaxEntries; ++k)
        if (f.entries[k].used && f.entries[k].enabled) add(f.entries[k], static_cast<int>(k));
    if (prog_.size() > kMaxProgramWords) {
        prog_.clear();
        return false;   // a console loads nothing, master included, until toggles bring it under 0x400
    }
    // decode front to back
    size_t ip = 0;
    while (ip < prog_.size()) {
        Ins in;
        in.at = ip;
        in.word0 = prog_[ip];
        const uint32_t w0 = prog_[ip];
        uint16_t top = static_cast<uint16_t>(w0 >> 28);
        auto need = [&](size_t count) -> bool { return ip + count <= prog_.size(); };
        bool bad = false;
        auto word = [&](size_t k) { return prog_[ip + k]; };

        if (top <= 0xA) {
            in.kind = top;
        } else if (top == 0xC) {
            const uint16_t sub = static_cast<uint16_t>((w0 >> 24) & 0xF);
            if (sub <= 4) in.kind = static_cast<uint16_t>(0xC0 + sub);
            else bad = true;   // C5..CF invalid
        } else if (top == 0xF) {
            const uint16_t sub = static_cast<uint16_t>((w0 >> 24) & 0xF);
            if (sub == 0xF) {
                const uint16_t sub2 = static_cast<uint16_t>((w0 >> 20) & 0xF);
                if (sub2 == 0x0) in.kind = 0xFF0;
                else if (sub2 == 0x1) in.kind = 0xFF1;
                else if (sub2 == 0xF) in.kind = 0xFFF;
                else bad = true;   // FF2..FFE invalid
            } else {
                bad = true;        // F00..FEF invalid
            }
        } else {
            bad = true;            // B, D, E
        }
        if (bad) {
            char why[64];
            std::snprintf(why, sizeof why, "unknown opcode 0x%08X", w0);
            invalid_why_ = why;
            break;
        }

        // field decode and length, per opcode
        size_t words = 1;
        switch (in.kind) {
            case 0x0: {   // 0TMR00AA AAAAAAAA VVVVVVVV [VVVVVVVV]
                in.w = static_cast<uint8_t>((w0 >> 24) & 0xF);
                in.a = static_cast<uint8_t>((w0 >> 20) & 0xF);   // M
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R
                words = 2 + (in.w == 8 ? 2 : 1);
                if (!need(words)) { bad = true; break; }
                in.imm = (static_cast<uint64_t>(w0 & 0xFF) << 32) | word(1);
                in.val = in.w == 8 ? ((static_cast<uint64_t>(word(2)) << 32) | word(3)) : word(2);
                break;
            }
            case 0x1: {   // 1TMCXrAA AAAAAAAA VVVVVVVV [VVVVVVVV]
                in.w = static_cast<uint8_t>((w0 >> 24) & 0xF);
                in.a = static_cast<uint8_t>((w0 >> 20) & 0xF);   // M
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // C (condition)
                in.c = static_cast<uint8_t>((w0 >> 12) & 0xF);   // X (use reg?)
                in.d = static_cast<uint8_t>((w0 >> 8) & 0xF);    // r
                words = 2 + (in.w == 8 ? 2 : 1);
                if (!need(words)) { bad = true; break; }
                in.imm = (static_cast<uint64_t>(w0 & 0xFF) << 32) | word(1);
                in.val = in.w == 8 ? ((static_cast<uint64_t>(word(2)) << 32) | word(3)) : word(2);
                break;
            }
            case 0x2:     // 20000000 end / 21000000 else
                in.a = static_cast<uint8_t>((w0 >> 24) & 0xF);
                break;
            case 0x3:     // 300R0000 NNNNNNNN  /  310R0000
                in.b = static_cast<uint8_t>((w0 >> 24) & 0xF);   // start(0)/end(1)
                in.c = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R (digit 3)
                if (in.b == 0) { words = 2; if (!need(words)) { bad = true; break; } in.val = word(1); }
                break;
            case 0x4:     // 400R0000 VVVVVVVV VVVVVVVV
                in.c = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R (digit 3)
                words = 3;
                if (!need(words)) { bad = true; break; }
                in.val = (static_cast<uint64_t>(word(1)) << 32) | word(2);
                break;
            case 0x5:     // 5TMRI0AA AAAAAAAA
                in.w = static_cast<uint8_t>((w0 >> 24) & 0xF);
                in.a = static_cast<uint8_t>((w0 >> 20) & 0xF);   // M
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R
                in.c = static_cast<uint8_t>((w0 >> 12) & 0xF);   // I
                in.d = static_cast<uint8_t>((w0 >> 8) & 0xF);    // S (I==2)
                words = 2;
                if (!need(words)) { bad = true; break; }
                in.imm = (static_cast<uint64_t>(w0 & 0xFF) << 32) | word(1);
                break;
            case 0x6:     // 6T0RIor0 VVVVVVVV VVVVVVVV
                in.w = static_cast<uint8_t>((w0 >> 24) & 0xF);
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R
                in.c = static_cast<uint8_t>((w0 >> 12) & 0xF);   // I (increment)
                in.d = static_cast<uint8_t>((w0 >> 8) & 0xF);    // o (offset reg?)
                in.e = static_cast<uint8_t>((w0 >> 4) & 0xF);    // r
                words = 3;
                if (!need(words)) { bad = true; break; }
                in.val = (static_cast<uint64_t>(word(1)) << 32) | word(2);
                break;
            case 0x7:     // 7T0RC000 VVVVVVVV
                in.w = static_cast<uint8_t>((w0 >> 24) & 0xF);
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R
                in.c = static_cast<uint8_t>((w0 >> 12) & 0xF);   // C (op)
                words = 2;
                if (!need(words)) { bad = true; break; }
                in.val = word(1);
                break;
            case 0x8:     // 8kkkkkkk key mask (28 bits)
                in.val = w0 & 0x0FFFFFFFu;
                break;
            case 0x9: {   // 9TCRS0s0  or  9TCRS100 VVVVVVVV [VVVVVVVV]
                in.w = static_cast<uint8_t>((w0 >> 24) & 0xF);
                in.b = static_cast<uint8_t>((w0 >> 20) & 0xF);   // C (op)
                in.c = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R dst
                in.d = static_cast<uint8_t>((w0 >> 12) & 0xF);   // S src
                in.e = static_cast<uint8_t>((w0 >> 8) & 0xF);    // immediate flag (1) or 0
                if (in.e != 0) {
                    words = 1 + (in.w == 8 ? 2 : 1);
                    if (!need(words)) { bad = true; break; }
                    in.val = in.w == 8 ? ((static_cast<uint64_t>(word(1)) << 32) | word(2)) : word(1);
                } else {
                    in.a = static_cast<uint8_t>((w0 >> 4) & 0xF);   // s (second source reg)
                }
                break;
            }
            case 0xA:     // ATSRIOxa (aaaaaaaa)
                in.w = static_cast<uint8_t>((w0 >> 24) & 0xF);
                in.d = static_cast<uint8_t>((w0 >> 20) & 0xF);   // S (value source)
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // R (address base)
                in.c = static_cast<uint8_t>((w0 >> 12) & 0xF);   // I (increment)
                in.e = static_cast<uint8_t>((w0 >> 8) & 0xF);    // O (address mode)
                in.a = static_cast<uint8_t>((w0 >> 4) & 0xF);    // x (reg for O 1/3/4/5)
                // offset present for O 2,4,5
                if (in.e == 2 || in.e == 4 || in.e == 5) {
                    words = 2;
                    if (!need(words)) { bad = true; break; }
                    in.imm = (static_cast<uint64_t>(w0 & 0xF) << 32) | word(1);
                }
                break;
            case 0xC0: {  // C0TcSX..  (digits: C 0 T c S X d6 d7)
                in.w = static_cast<uint8_t>((w0 >> 20) & 0xF);   // T  (digit 2)
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // c  condition (digit 3)
                in.c = static_cast<uint8_t>((w0 >> 12) & 0xF);   // S  reg to compare (digit 4)
                in.d = static_cast<uint8_t>((w0 >> 8) & 0xF);    // X  operand type (digit 5)
                in.e = static_cast<uint8_t>((w0 >> 4) & 0xF);    // M / R / X-reg (digit 6)
                in.a = static_cast<uint8_t>(w0 & 0xF);           // r (digit 7)
                if (in.d == 0 || in.d == 2) {                    // immediate offset: base(M)+a or reg[R]+a
                    words = 2;
                    if (!need(words)) { bad = true; break; }
                    in.imm = (static_cast<uint64_t>(w0 & 0xF) << 32) | word(1);
                } else if (in.d == 4) {                          // static value of T bytes
                    words = 1 + (in.w == 8 ? 2 : 1);
                    if (!need(words)) { bad = true; break; }
                    in.val = in.w == 8 ? ((static_cast<uint64_t>(word(1)) << 32) | word(2)) : word(1);
                }
                break;
            }
            case 0xC1:    // C10D0Sx0 save/restore one
                in.b = static_cast<uint8_t>((w0 >> 16) & 0xF);   // D
                in.c = static_cast<uint8_t>((w0 >> 8) & 0xF);    // S
                in.d = static_cast<uint8_t>((w0 >> 4) & 0xF);    // x (direction)
                break;
            case 0xC2:    // C2x0MMMM save/restore by mask
                in.d = static_cast<uint8_t>((w0 >> 20) & 0xF);   // x (direction, digit 2)
                in.val = w0 & 0xFFFF;                            // mask (digits 4..7)
                break;
            case 0xC3:    // C3000XXx static registers
                in.val = (w0 >> 4) & 0xFF;                       // XX index
                in.c = static_cast<uint8_t>(w0 & 0xF);           // x reg
                break;
            case 0xC4:    // C4r00000 kkkkkkkk kkkkkkkk  (64-bit mask)
                in.a = static_cast<uint8_t>((w0 >> 20) & 0xF);   // r (auto-repeat, digit 2)
                words = 3;
                if (!need(words)) { bad = true; break; }
                in.val = (static_cast<uint64_t>(word(1)) << 32) | word(2);
                break;
            case 0xFF0:   // break
            case 0xFF1:   // continue
                break;
            case 0xFFF:   // FFFTIX..  (digits: F F F T I X d6 d7) one value logged
                in.w = static_cast<uint8_t>((w0 >> 16) & 0xF);   // T  (digit 3)
                in.c = static_cast<uint8_t>((w0 >> 12) & 0xF);   // I  log id (digit 4)
                in.d = static_cast<uint8_t>((w0 >> 8) & 0xF);    // X  operand type (digit 5)
                in.e = static_cast<uint8_t>((w0 >> 4) & 0xF);    // M / R / X-reg (digit 6)
                in.b = static_cast<uint8_t>(w0 & 0xF);           // r (digit 7)
                if (in.d == 0 || in.d == 2) {
                    words = 2;
                    if (!need(words)) { bad = true; break; }
                    in.imm = (static_cast<uint64_t>(w0 & 0xF) << 32) | word(1);
                }
                break;
            default:
                bad = true;
                break;
        }
        if (bad) {
            char why[96];
            std::snprintf(why, sizeof why, "an instruction's last words are missing (0x%08X); the pass ends there", w0);
            invalid_why_ = why;
            break;
        }
        in.next = ip + words;
        ins_.push_back(in);
        ip += words;
    }
    valid_ = ins_.size();
    loaded_ = true;
    return true;
}

size_t Vm::ins_index_of_word(size_t word) const {
    // binary search on in.at (sorted)
    size_t lo = 0, hi = valid_;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (ins_[mid].at < word) lo = mid + 1;
        else hi = mid;
    }
    return (lo < valid_ && ins_[lo].at == word) ? lo : valid_;
}

int Vm::entry_at(size_t ip) const {
    int idx = -1;
    for (const auto& s : spans_)
        if (s.first <= ip) idx = s.second;
        else break;
    return idx;
}

int Vm::writing_entry() const {
    if (writing_at_ == SIZE_MAX) return -1;
    const int k = entry_at(writing_at_);
    return k >= 0 && static_cast<size_t>(k) < file_entries_.size() ? file_entries_[static_cast<size_t>(k)] : -1;
}

void Vm::reset_static() {
    for (uint64_t& v : static_) v = 0;
}

namespace {
uint64_t mask_of(unsigned width) {
    switch (width) {
        case 1: return 0xFFull;
        case 2: return 0xFFFFull;
        case 4: return 0xFFFFFFFFull;
        case 8: return ~0ull;
        default: return 0;   // T outside 1/2/4/8: a no-op access, value 0
    }
}
bool compare(unsigned cond, uint64_t a, uint64_t b) {
    switch (cond) {
        case 1: return a > b;
        case 2: return a >= b;
        case 3: return a < b;
        case 4: return a <= b;
        case 5: return a == b;
        case 6: return a != b;
        default: return false;
    }
}
}  // namespace

uint64_t Vm::mem_read(VmHost& host, uint64_t addr, unsigned width, PassReport& rep) {
    const uint64_t m = mask_of(width);
    if (m == 0) return 0;
    const size_t n = width;
    uint64_t v = 0;
    if (!host.read(addr, &v, n)) {
        rep.failed_reads++;
        return 0;
    }
    return v & m;
}

bool Vm::mem_write(VmHost& host, uint64_t addr, uint64_t value, unsigned width, PassReport& rep, size_t at) {
    if (mask_of(width) == 0) return true;   // width not 1/2/4/8: no-op
    writing_at_ = at;
    const bool ok = host.write(addr, &value, width);
    writing_at_ = SIZE_MAX;
    if (!ok) {
        rep.failed_writes++;
        return false;
    }
    rep.writes++;
    if (on_write) on_write(addr, &value, width, on_write_user);
    return true;
}


uint64_t Vm::c0_operand(VmHost& host, const Ins& in, PassReport& rep) {
    const uint64_t m = mask_of(in.w) ? mask_of(in.w) : ~0ull;
    switch (in.d) {
        case 0: return mem_read(host, host.base(in.e) + in.imm, in.w, rep);     // base(M)+a      (M=in.e)
        case 1: return mem_read(host, host.base(in.e) + reg_[in.a], in.w, rep); // base(M)+reg[r] (M=in.e, r=in.a)
        case 2: return mem_read(host, reg_[in.e] + in.imm, in.w, rep);          // reg[R]+a       (R=in.e)
        case 3: return mem_read(host, reg_[in.e] + reg_[in.a], in.w, rep);      // reg[R]+reg[r]
        case 4: return in.val & m;                                              // static value
        case 5: return reg_[in.e] & m;                                          // reg[X]         (X=in.e)
        default: return 0;
    }
}

namespace {
bool opens_block(uint16_t kind) { return kind == 0x1 || kind == 0x8 || kind == 0xC0 || kind == 0xC4; }
}  // namespace

// An unmet conditional at `from` (just after the conditional word): walk to the matching else (entered_else = true, the else body runs)
// or the matching end (entered_else = false), respecting nested conditionals. Returns the resume instruction index.
size_t Vm::skip_conditional(size_t from, bool& entered_else) const {
    entered_else = false;
    size_t nest = 0;
    for (size_t pc = from; pc < valid_; ++pc) {
        const Ins& in = ins_[pc];
        if (opens_block(in.kind)) { ++nest; continue; }
        if (in.kind == 0x2) {
            if (in.a == 1) {                 // else
                if (nest == 0) { entered_else = true; return pc + 1; }
                continue;
            }
            if (nest == 0) return pc + 1;    // end
            --nest;
        }
    }
    return valid_;
}

// An else reached while executing a taken branch: skip to its matching end, respecting nesting. Returns the resume index.
size_t Vm::skip_to_end(size_t from) const {
    size_t nest = 0;
    for (size_t pc = from; pc < valid_; ++pc) {
        const Ins& in = ins_[pc];
        if (opens_block(in.kind)) { ++nest; continue; }
        if (in.kind == 0x2 && in.a != 1) {
            if (nest == 0) return pc + 1;
            --nest;
        }
    }
    return valid_;
}

PassReport Vm::run(VmHost& host) {
    PassReport rep;
    if (!loaded_ || ins_.empty()) return rep;
    rep.ran = true;
    for (uint64_t& r : reg_) r = 0;
    for (uint64_t& s : saved_) s = 0;
    for (size_t& l : loop_top_) l = 0;
    size_t depth = 0;
    const uint64_t keys = host.keys_held();
    bool reached_tail = false;         // execution fell through to a word that could not be decoded

    size_t pc = 0;
    while (pc < valid_) {
        if (rep.steps++ >= max_steps) {
            rep.stopped = true;
            rep.why = "the pass ran over 4,000,000 instructions; a console's cheat thread would hang here";
            break;
        }
        const Ins& in = ins_[pc];
        size_t next = pc + 1;

        // A conditional tested here (1, 8, C0, C4): met -> enter the block (depth++); unmet -> skip it (to else or end).
        auto handle_cond = [&](bool met) {
            if (met) { ++depth; return; }
            bool entered_else = false;
            const size_t resume = skip_conditional(pc + 1, entered_else);
            if (entered_else) ++depth;     // the else body is its own open block, closed by its end
            next = resume;
        };

        switch (in.kind) {
            case 0x0:
                mem_write(host, host.base(in.a) + in.imm + reg_[in.b], in.val, in.w, rep, in.at);
                break;
            case 0x1: {
                uint64_t addr = host.base(in.a) + in.imm;
                if (in.c != 0) addr += reg_[in.d];
                handle_cond(compare(in.b, mem_read(host, addr, in.w, rep), in.val));
                break;
            }
            case 0x2:
                if (in.a == 1) {           // else reached while executing a taken branch
                    if (depth == 0) {
                        rep.stopped = true;
                        rep.why = "an else at depth 0; a console's dmnt aborts here";
                        next = valid_;
                        break;
                    }
                    --depth;
                    next = skip_to_end(pc + 1);
                } else {
                    if (depth > 0) --depth; // end at depth 0 is a no-op
                }
                break;
            case 0x3:
                if (in.b == 0) {           // loop start
                    reg_[in.c] = in.val;
                    loop_top_[in.c] = in.next;
                } else {                   // loop end
                    reg_[in.c]--;
                    if (reg_[in.c] != 0) {
                        const size_t idx = ins_index_of_word(loop_top_[in.c]);
                        if (idx < valid_) next = idx;
                    }
                }
                break;
            case 0x4:
                reg_[in.c] = in.val;
                break;
            case 0x5: {
                uint64_t addr;
                if (in.c == 1) addr = reg_[in.b] + in.imm;
                else if (in.c == 2) addr = reg_[in.d] + in.imm;
                else if (in.c == 3) addr = host.base(in.a) + reg_[in.d] + in.imm;
                else addr = host.base(in.a) + in.imm;
                const uint64_t m = mask_of(in.w);
                if (m) {
                    uint64_t v = 0;
                    if (host.read(addr, &v, in.w)) reg_[in.b] = (reg_[in.b] & ~m) | (v & m);
                    else rep.failed_reads++;
                }
                break;
            }
            case 0x6:
                mem_write(host, reg_[in.b] + (in.d != 0 ? reg_[in.e] : 0), in.val, in.w, rep, in.at);
                if (in.c != 0 && mask_of(in.w)) reg_[in.b] += in.w;
                break;
            case 0x7: {
                uint64_t& r = reg_[in.b];   // 7's R is nybble 4 (decode stored it in in.b)
                const uint64_t v = in.val;
                switch (in.c) {             // C (op) in nybble 3
                    case 0: r += v; break;
                    case 1: r -= v; break;
                    case 2: r *= v; break;
                    case 3: r <<= (v & 63); break;
                    case 4: r >>= (v & 63); break;
                    default: break;
                }
                const uint64_t m = mask_of(in.w);
                if (m) r &= m;
                break;
            }
            case 0x8:
                handle_cond((in.val & keys) == in.val);
                break;
            case 0x9: {
                const unsigned op = in.b;
                const uint64_t s = reg_[in.d];
                const uint64_t operand = in.e != 0 ? in.val : reg_[in.a];
                uint64_t res = 0;
                const uint64_t m = mask_of(in.w);
                switch (op) {
                    case 0: res = s + operand; break;
                    case 1: res = s - operand; break;
                    case 2: res = s * operand; break;
                    case 3: res = s << (operand & 63); break;
                    case 4: res = s >> (operand & 63); break;
                    case 5: res = s & operand; break;
                    case 6: res = s | operand; break;
                    case 7: res = ~s; break;
                    case 8: res = s ^ operand; break;
                    case 9: res = s; break;
                    case 10: case 11: case 12: case 13:
                        if (in.w == 4) {
                            float a, b2;
                            std::memcpy(&a, &s, 4);
                            std::memcpy(&b2, &operand, 4);
                            const float f = op == 10 ? a + b2 : op == 11 ? a - b2 : op == 12 ? a * b2 : (b2 != 0 ? a / b2 : 0.f);
                            uint32_t u;
                            std::memcpy(&u, &f, 4);
                            res = u;
                        } else if (in.w == 8) {
                            double a, b2;
                            std::memcpy(&a, &s, 8);
                            std::memcpy(&b2, &operand, 8);
                            const double f = op == 10 ? a + b2 : op == 11 ? a - b2 : op == 12 ? a * b2 : (b2 != 0 ? a / b2 : 0.0);
                            std::memcpy(&res, &f, 8);
                        }
                        break;
                    default: res = 0; break;
                }
                reg_[in.c] = m ? (res & m) : res;
                break;
            }
            case 0xA: {
                uint64_t addr;
                switch (in.e) {
                    case 0: addr = reg_[in.b]; break;
                    case 1: addr = reg_[in.b] + reg_[in.a]; break;
                    case 2: addr = reg_[in.b] + in.imm; break;
                    case 3: addr = host.base(in.a) + reg_[in.b]; break;
                    case 4: addr = host.base(in.a) + in.imm; break;
                    case 5: addr = host.base(in.a) + reg_[in.b] + in.imm; break;
                    default: addr = reg_[in.b]; break;
                }
                mem_write(host, addr, reg_[in.d], in.w, rep, in.at);
                if (in.c != 0 && mask_of(in.w)) reg_[in.b] += in.w;
                break;
            }
            case 0xC0: {
                const uint64_t m = mask_of(in.w) ? mask_of(in.w) : ~0ull;
                const uint64_t lhs = reg_[in.c] & m;
                handle_cond(compare(in.b, lhs, c0_operand(host, in, rep) & m));
                break;
            }
            case 0xC1:
                switch (in.d) {
                    case 0: reg_[in.b] = saved_[in.c]; break;
                    case 1: saved_[in.b] = reg_[in.c]; break;
                    case 2: saved_[in.b] = 0; break;
                    case 3: reg_[in.b] = 0; break;
                    default: reg_[in.b] = saved_[in.c]; break;
                }
                break;
            case 0xC2:
                for (int i = 0; i < 16; ++i) {
                    if (!((in.val >> i) & 1)) continue;
                    switch (in.d) {
                        case 0: reg_[i] = saved_[i]; break;
                        case 1: saved_[i] = reg_[i]; break;
                        case 2: saved_[i] = 0; break;
                        case 3: reg_[i] = 0; break;
                        default: reg_[i] = saved_[i]; break;
                    }
                }
                break;
            case 0xC3: {
                const unsigned idx = static_cast<unsigned>(in.val);
                if (idx < 0x80) reg_[in.c] = static_[idx];
                else static_[idx] = reg_[in.c];
                break;
            }
            case 0xC4: {
                const bool met = in.a != 0 ? (in.val & keys) == in.val
                                           : (in.val & keys) == in.val && (in.val & keyold_) != in.val;
                handle_cond(met);
                break;
            }
            case 0xFF0: host.pause_game(true); break;
            case 0xFF1: host.pause_game(false); break;
            case 0xFFF: {
                uint64_t v = 0;
                switch (in.d) {
                    case 0: v = mem_read(host, host.base(in.e) + in.imm, in.w, rep); break;        // base(M)+a
                    case 1: v = mem_read(host, host.base(in.e) + reg_[in.b], in.w, rep); break;    // base(M)+reg[r]
                    case 2: v = mem_read(host, reg_[in.e] + in.imm, in.w, rep); break;             // reg[R]+a
                    case 3: v = mem_read(host, reg_[in.e] + reg_[in.b], in.w, rep); break;         // reg[R]+reg[r]
                    case 4: v = reg_[in.e] & (mask_of(in.w) ? mask_of(in.w) : ~0ull); break;       // reg[X]
                    default: break;
                }
                host.debug_log(in.c, v);
                break;
            }
            default:
                break;
        }

        if (next == valid_ && valid_ < prog_.size() && !invalid_why_.empty()) reached_tail = true;
        pc = next;
    }

    keyold_ = keys;
    // Execution fell through to a word that could not be decoded (unknown opcode or an instruction cut short): a console stops the pass
    // there, and the later cheats do not run. A conditional that skipped past the end is a clean finish instead.
    if (!rep.stopped && reached_tail) {
        rep.stopped = true;
        rep.why = invalid_why_;
    }
    return rep;
}

}  // namespace nxrt::cheats
