// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// [nvn] shader_key_check, see nvn_key_check.h.
#include "nvn_key_check.h"

#include <cstdio>

#include "nvn_shaders.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::nvn {

namespace {
constexpr uint32_t kMaxHashBytes = 16u << 20;   // as api_program.cpp: a corrupt control section must not make us read far

uint64_t hex_key(const std::string& hex) {
    uint64_t v = 0;
    for (char ch : hex) {
        uint64_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint64_t>(ch - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}
}  // namespace

bool rehash_block(const uint8_t* code, const uint8_t* control, uint64_t* key) {
    if (!code || !control) return false;
    const be::ShaderControl c = be::read_control(control);
    if (!c.valid || c.code_size == 0 || c.code_size > kMaxHashBytes) return false;
    *key = hex_key(be::shader_hash(code, c.code_size));
    return true;
}

int KeyChecker::on_draw(const KeySnapshot& s, uint64_t frame, std::string* line) {
    if (line) line->clear();
    auto ins = seen_.try_emplace(s.id);
    Entry& e = ins.first->second;
    if (ins.second) ++programs_;
    const uint32_t n = e.draws++;
    if (n != 0 && (every_ <= 1 || n % every_ != 0)) return 0;
    ++checks_;
    int bad = 0;
    char buf[160];
    std::string text;
    for (int st = 0; st < 6; ++st) {
        const KeyStage& k = s.st[st];
        if (!k.code || !k.control || k.key == 0) continue;
        uint64_t now = 0;
        if (!fn_(k.code, k.control, &now) || now == k.key) continue;
        ++bad;
        ++bad_stages_;
        if (!e.logged) {
            std::snprintf(buf, sizeof buf, "%sstage %d key %016llx -> %016llx", text.empty() ? "" : "; ", st,
                          static_cast<unsigned long long>(k.key), static_cast<unsigned long long>(now));
            text += buf;
        }
    }
    if (bad && !e.logged) {
        e.logged = true;
        ++bad_programs_;
        std::snprintf(buf, sizeof buf, "shader_key_check: program %p, first draw frame %llu (draw %u): ", reinterpret_cast<void*>(s.id),
                      static_cast<unsigned long long>(frame), n + 1);
        if (line) *line = buf + text;
    }
    return bad;
}

namespace {
struct Global {
    const bool on;
    KeyChecker checker;
    KeySnapshot bound;
    bool have = false;
    uint64_t reported = 0;
    Global(int64_t v) : on(v > 0), checker(v > 1 ? static_cast<uint32_t>(v) : 1) {}
};
Global& G() {
    static Global g(nxrt::config_get_int("nvn", "shader_key_check", 0));
    return g;
}
}  // namespace

bool key_check_on() {
    static const bool on = G().on;   // read on first use (the config is loaded by then); afterwards a guard load and a branch
    return on;
}

void key_check_bind(uintptr_t program_id, const KeyStage stages[6]) {
    Global& g = G();
    g.bound.id = program_id;
    for (int i = 0; i < 6; ++i) g.bound.st[i] = stages[i];
    g.have = program_id != 0;
}

void key_check_draw(uint64_t frame) {
    Global& g = G();
    if (!g.have) return;
    std::string line;
    g.checker.on_draw(g.bound, frame, &line);
    if (!line.empty()) {
        NXLOG_W("nvn", "%s", line.c_str());
        NXLOG_W("nvn", "shader_key_check: %llu of %llu programs changed so far (%llu stages)",
                static_cast<unsigned long long>(g.checker.programs_mismatched()), static_cast<unsigned long long>(g.checker.programs_checked()),
                static_cast<unsigned long long>(g.checker.stages_mismatched()));
    }
}

}  // namespace nxrt::nvn
