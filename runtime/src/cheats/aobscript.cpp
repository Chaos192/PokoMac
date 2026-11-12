// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Cheat Engine-style AOB scripts (aobscript.h).
#include "aobscript.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "aob.h"
#include "disasm.h"

namespace nxrt::cheats::aa {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
// Splits "a, b, c" on top-level commas.
std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if (c == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}
// "name(args)" -> name, args. False when the line is not a call.
bool as_call(const std::string& line, std::string* name, std::string* args) {
    const size_t p = line.find('(');
    if (p == std::string::npos || line.back() != ')') return false;
    const std::string n = trim(line.substr(0, p));
    for (char c : n) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    if (n.empty()) return false;
    *name = lower(n);
    *args = line.substr(p + 1, line.size() - p - 2);
    return true;
}

bool parse_number(const std::string& t, uint64_t* out) {
    if (t.empty()) return false;
    const char* s = t.c_str();
    char* end = nullptr;
    if (s[0] == '#') {                              // CE: #decimal
        if (!s[1]) return false;
        *out = std::strtoull(s + 1, &end, 10);
        return *end == 0;
    }
    if (t.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!std::isxdigit(static_cast<unsigned char>(*s))) return false;
    *out = std::strtoull(s, &end, 16);              // CE's default radix in scripts is hex
    return *end == 0;
}

// The first match of `pat` in [lo, hi), read through src in chunks (a match straddling two chunks is found). 0 = none.
uint64_t find_first(MemSource& src, const AobPattern& pat, uint64_t lo, uint64_t hi) {
    if (pat.empty() || hi <= lo || hi - lo < pat.size()) return 0;
    const size_t chunk = 1 << 20;
    std::vector<uint8_t> buf;
    for (uint64_t a = lo; a < hi; a += chunk) {
        const uint64_t end = std::min<uint64_t>(hi, a + chunk + pat.size() - 1);
        buf.resize(static_cast<size_t>(end - a));
        if (!src.read(a, buf.data(), buf.size())) {
            // part of it is unreadable: try page by page
            for (uint64_t pg = a; pg < std::min<uint64_t>(hi, a + chunk); pg += 0x1000) {
                const uint64_t pe = std::min<uint64_t>(hi, pg + 0x1000 + pat.size() - 1);
                std::vector<uint8_t> pb(static_cast<size_t>(pe - pg));
                if (!src.read(pg, pb.data(), pb.size())) continue;
                for (size_t i = 0; i + pat.size() <= pb.size() && pg + i < pg + 0x1000; ++i)
                    if (pat.matches(pb.data() + i)) return pg + i;
            }
            continue;
        }
        for (size_t i = 0; i + pat.size() <= buf.size() && a + i < a + chunk; ++i)
            if (pat.matches(buf.data() + i)) return a + i;
    }
    return 0;
}

struct Runner {
    ScriptHost& host;
    RunResult res;
    std::map<std::string, uint64_t> local;
    uint64_t cur = 0;
    bool have_cur = false;
    int line = 0;

    bool fail(const std::string& m) {
        char b[32];
        std::snprintf(b, sizeof b, "line %d: ", line);
        res.error = b + m;
        return false;
    }
    bool ev(const std::string& e, uint64_t* out) {
        std::string err;
        if (!eval(e, host, local, out, &err)) return fail(err);
        return true;
    }
    bool emit(const std::vector<uint8_t>& bytes) {
        if (!have_cur) return fail("no write address yet (a line `<address>:` comes first)");
        Write w;
        w.addr = cur;
        w.bytes = bytes;
        w.original.resize(bytes.size());
        if (!host.mem().read(cur, w.original.data(), bytes.size())) {
            char b[64];
            std::snprintf(b, sizeof b, "0x%llx is not readable guest memory", static_cast<unsigned long long>(cur));
            return fail(b);
        }
        std::string err;
        if (!host.write(w, &err)) return fail(err.empty() ? "the write was refused" : err);
        res.writes.push_back(w);
        cur += bytes.size();
        return true;
    }
    bool scan(const std::string& name, const std::string& where, uint64_t lo, uint64_t hi, bool anywhere, const std::string& pattern) {
        AobPattern pat;
        std::string err;
        if (!aob_parse(pattern, &pat, &err)) return fail("aobscan " + name + ": " + err);
        uint64_t hit = 0;
        if (anywhere) {
            RegionFilter f;
            f.main = f.heap = f.alias = f.code = f.other = true;
            for (const ScanRegion& r : host.mem().regions(f))
                if ((hit = find_first(host.mem(), pat, r.addr, r.addr + r.size))) break;
        } else {
            hit = find_first(host.mem(), pat, lo, hi);
        }
        if (!hit) return fail("aobscan " + name + ": the pattern is not in " + where + " (the script was made for another build of the game?)");
        local[lower(name)] = hit;
        res.names[name] = hit;
        return true;
    }

    bool statement(const std::string& raw) {
        std::string s = trim(raw);
        if (s.empty()) return true;
        std::string fn, args;
        if (as_call(s, &fn, &args)) {
            const auto a = split_args(args);
            if (fn == "aobscan") {
                if (a.size() != 2) return fail("aobscan(name, pattern)");
                return scan(a[0], "the game's memory", 0, 0, true, a[1]);
            }
            if (fn == "aobscanmodule") {
                if (a.size() != 3) return fail("aobscanmodule(name, module, pattern)");
                uint64_t lo = 0, hi = 0;
                if (!host.module_range(a[1], &lo, &hi)) return fail("aobscanmodule " + a[0] + ": no module named " + a[1]);
                return scan(a[0], a[1], lo, hi, false, a[2]);
            }
            if (fn == "aobscanregion") {
                if (a.size() != 4) return fail("aobscanregion(name, start, end, pattern)");
                uint64_t lo = 0, hi = 0;
                if (!ev(a[1], &lo) || !ev(a[2], &hi)) return false;
                return scan(a[0], "the region", lo, hi, false, a[3]);
            }
            if (fn == "define") {
                if (a.size() != 2) return fail("define(name, value)");
                uint64_t v = 0;
                if (!ev(a[1], &v)) return false;
                local[lower(a[0])] = v;
                res.names[a[0]] = v;
                return true;
            }
            if (fn == "label" || fn == "alloc" || fn == "dealloc") {
                if (fn != "label") return fail(fn + ": code caves are made through the code patcher (the overlay's Inject), not in a script");
                return true;
            }
            if (fn == "registersymbol") {
                for (const auto& n : a) {
                    auto it = local.find(lower(n));
                    if (it == local.end()) return fail("registersymbol " + n + ": not defined in this script");
                    host.symbols()[lower(n)] = it->second;
                }
                return true;
            }
            if (fn == "unregistersymbol") {
                for (const auto& n : a) host.symbols().erase(lower(n));
                return true;
            }
            if (fn == "assert") {
                if (a.size() != 2) return fail("assert(address, bytes)");
                uint64_t at = 0;
                if (!ev(a[0], &at)) return false;
                AobPattern pat;
                std::string err;
                if (!aob_parse(a[1], &pat, &err)) return fail("assert: " + err);
                std::vector<uint8_t> have(pat.size());
                if (!host.mem().read(at, have.data(), have.size()) || !pat.matches(have.data()))
                    return fail("assert: the bytes at " + a[0] + " are not " + a[1] + " (another build of the game?)");
                return true;
            }
            if (fn == "readmem") {
                if (a.size() != 2) return fail("readmem(address, count)");
                uint64_t from = 0, n = 0;
                if (!ev(a[0], &from) || !ev(a[1], &n)) return false;
                if (n > 0x10000) return fail("readmem: more than 64 KiB");
                std::vector<uint8_t> b(static_cast<size_t>(n));
                if (!host.mem().read(from, b.data(), b.size())) return fail("readmem: not readable");
                return emit(b);
            }
            // anything else that looks like a call is an instruction such as "b.eq(..." - not ours: fall through
        }
        // "<expr>:" sets the address
        if (s.back() == ':' && s.find(' ') == std::string::npos) {
            uint64_t v = 0;
            if (!ev(s.substr(0, s.size() - 1), &v)) return false;
            cur = v;
            have_cur = true;
            return true;
        }
        // data directives
        const size_t sp = s.find_first_of(" \t");
        const std::string op = lower(sp == std::string::npos ? s : s.substr(0, sp));
        const std::string rest = sp == std::string::npos ? std::string() : trim(s.substr(sp + 1));
        auto values = [&](size_t width) -> bool {
            std::vector<uint8_t> out;
            std::string tok;
            std::vector<std::string> toks;
            for (char c : rest + " ") {
                if (std::isspace(static_cast<unsigned char>(c)) || c == ',') { if (!tok.empty()) toks.push_back(tok); tok.clear(); }
                else tok += c;
            }
            if (toks.empty()) return fail(op + " needs values");
            for (const auto& t : toks) {
                uint64_t v = 0;
                if (!parse_number(t, &v)) {
                    if (!ev(t, &v)) return false;
                }
                for (size_t k = 0; k < width; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
            }
            return emit(out);
        };
        if (op == "db") return values(1);
        if (op == "dw") return values(2);
        if (op == "dd") return values(4);
        if (op == "dq") return values(8);
        // an instruction: branch targets may be expressions (b coindec+10)
        std::string line_text = s;
        if ((op == "b" || op == "bl") && !rest.empty() && rest[0] != '#') {
            uint64_t t = 0;
            if (!ev(rest, &t)) return false;
            char b[48];
            std::snprintf(b, sizeof b, "%s 0x%llx", op.c_str(), static_cast<unsigned long long>(t));
            line_text = b;
        }
        uint32_t word = 0;
        std::string err;
        if (!a64::assemble(line_text, cur, &word, &err)) return fail("'" + s + "': " + err);
        std::vector<uint8_t> b(4);
        std::memcpy(b.data(), &word, 4);
        return emit(b);
    }
};

std::string strip_comments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool brace = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (brace) {
            if (c == '}') brace = false;
            else if (c == '\n') out += c;       // keep the line count
            continue;
        }
        if (c == '{') { brace = true; continue; }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            if (i < text.size()) out += '\n';
            continue;
        }
        out += c;
    }
    return out;
}

}  // namespace

bool eval(const std::string& expr_in, ScriptHost& host, const std::map<std::string, uint64_t>& local, uint64_t* out, std::string* err) {
    const std::string expr = trim(expr_in);
    if (expr.empty()) { if (err) *err = "empty address"; return false; }
    uint64_t acc = 0;
    size_t i = 0;
    int sign = 1;
    bool any = false;
    while (i <= expr.size()) {
        size_t j = i;
        while (j < expr.size() && expr[j] != '+' && expr[j] != '-') ++j;
        const std::string term = trim(expr.substr(i, j - i));
        if (term.empty()) {
            if (j >= expr.size()) break;
            if (any) { if (err) *err = "'" + expr + "': two operators in a row"; return false; }
        } else {
            uint64_t v = 0;
            const std::string k = lower(term);
            if (auto it = local.find(k); it != local.end()) v = it->second;
            else if (auto it2 = host.symbols().find(k); it2 != host.symbols().end()) v = it2->second;
            else {
                uint64_t lo = 0, hi = 0;
                if (host.module_range(term, &lo, &hi)) v = lo;
                else if (!parse_number(term, &v)) { if (err) *err = "unknown name or number '" + term + "'"; return false; }
            }
            acc = sign > 0 ? acc + v : acc - v;
            any = true;
        }
        if (j >= expr.size()) break;
        sign = expr[j] == '+' ? 1 : -1;
        i = j + 1;
    }
    if (!any) { if (err) *err = "'" + expr + "' is not an address"; return false; }
    *out = acc;
    return true;
}

bool parse(const std::string& text_in, Script* out, std::string* err) {
    Script s;
    const std::string text = strip_comments(text_in);
    Section* cur = nullptr;
    bool seen_enable = false, any_header = text.find('[') != std::string::npos &&
                                          (lower(text).find("[enable]") != std::string::npos || lower(text).find("[disable]") != std::string::npos);
    if (!any_header) cur = &s.enable;
    int ln = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        ++ln;
        pos = nl + 1;
        if (!line.empty()) {
            const std::string l = lower(line);
            if (l == "[enable]") {
                if (seen_enable) { if (err) *err = "line " + std::to_string(ln) + ": a second [ENABLE]"; return false; }
                seen_enable = true;
                cur = &s.enable;
            } else if (l == "[disable]") {
                if (s.has_disable) { if (err) *err = "line " + std::to_string(ln) + ": a second [DISABLE]"; return false; }
                s.has_disable = true;
                cur = &s.disable;
            } else {
                if (!cur) { if (err) *err = "line " + std::to_string(ln) + ": text before [ENABLE]"; return false; }
                cur->lines.push_back(line);
                cur->line_no.push_back(ln);
            }
        }
        if (nl >= text.size()) break;
    }
    *out = std::move(s);
    return true;
}

RunResult run(const Section& s, ScriptHost& host, const std::map<std::string, uint64_t>& seed) {
    Runner r{host, {}, {}, 0, false, 0};
    for (const auto& kv : seed) r.local[lower(kv.first)] = kv.second;
    for (size_t i = 0; i < s.lines.size(); ++i) {
        r.line = s.line_no[i];
        if (!r.statement(s.lines[i])) return r.res;
    }
    r.res.ok = true;
    return r.res;
}

namespace {
void undo_writes(ScriptHost& host, const std::vector<Write>& ws) {
    for (auto it = ws.rbegin(); it != ws.rend(); ++it) {
        Write back;
        back.addr = it->addr;
        back.bytes = it->original;
        back.original = it->bytes;
        std::string e;
        host.write(back, &e);
    }
}
}  // namespace

RunResult enable(const Script& s, ScriptHost& host) {
    RunResult r = run(s.enable, host);
    if (!r.ok) undo_writes(host, r.writes);
    return r;
}

RunResult disable(const Script& s, ScriptHost& host, const RunResult& enabled) {
    if (s.has_disable && !s.disable.lines.empty()) {
        RunResult d = run(s.disable, host, enabled.names);
        // a [DISABLE] that writes nothing (only unregistersymbol, say) puts back what [ENABLE] wrote, as an empty one does: a patch
        // left in place would also hide the bytes the next [ENABLE] looks for
        if (d.ok && d.writes.empty()) undo_writes(host, enabled.writes);
        return d;
    }
    RunResult r;
    undo_writes(host, enabled.writes);
    r.ok = true;
    return r;
}

std::string make_patch_script(const std::string& name, const std::string& module, const std::string& signature, int64_t offset,
                              const std::vector<uint8_t>& new_bytes, const std::vector<uint8_t>& old_bytes) {
    std::string sym;
    for (char c : name) sym += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
    if (sym.empty() || std::isdigit(static_cast<unsigned char>(sym[0]))) sym = "p_" + sym;
    char off[32];
    if (offset >= 0) std::snprintf(off, sizeof off, "+%llX", static_cast<unsigned long long>(offset));
    else std::snprintf(off, sizeof off, "-%llX", static_cast<unsigned long long>(-offset));
    auto bytes = [](const std::vector<uint8_t>& b) {
        std::string s;
        char h[4];
        for (size_t i = 0; i < b.size(); ++i) {
            std::snprintf(h, sizeof h, "%s%02X", i ? " " : "", b[i]);
            s += h;
        }
        return s;
    };
    std::string s = "// " + name + " (made by the AnySwitch cheats overlay)\n[ENABLE]\n";
    s += "aobscanmodule(" + sym + ", " + module + ", " + signature + ")\n";
    s += sym + std::string(offset ? off : "") + ":\n";
    s += "  db " + bytes(new_bytes) + "\n";
    s += "registersymbol(" + sym + ")\n[DISABLE]\n";
    s += sym + std::string(offset ? off : "") + ":\n";
    s += "  db " + bytes(old_bytes) + "\n";
    s += "unregistersymbol(" + sym + ")\n";
    return s;
}

}  // namespace nxrt::cheats::aa
