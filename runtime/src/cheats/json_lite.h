// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// A minimal JSON reader: a recursive parser over a value tree, enough for the files the cheats code reads (the saved cheat table and
// layout, cheattable.cpp; the overlay's theme files, cheats_theme_file.cpp) and for hand edits. No third-party dependency.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::cheats::json {

struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
    const JVal* get(const char* key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool as_bool(bool d = false) const { return t == Bool ? b : t == Num ? num != 0 : d; }
    uint64_t as_u64(uint64_t d = 0) const {
        if (t == Num) return static_cast<uint64_t>(num);
        if (t == Str) return std::strtoull(str.c_str(), nullptr, 0);
        return d;
    }
    int64_t as_i64(int64_t d = 0) const {
        if (t == Num) return static_cast<int64_t>(num);
        if (t == Str) return std::strtoll(str.c_str(), nullptr, 0);
        return d;
    }
    int as_int(int d = 0) const { return static_cast<int>(as_i64(d)); }
    std::string as_str(const std::string& d = "") const { return t == Str ? str : d; }
};

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool parse(JVal* v) {
        ws();
        if (p >= end) return fail();
        char c = *p;
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v->t = JVal::Str; return string(&v->str); }
        if (c == 't' || c == 'f') return boolean(v);
        if (c == 'n') { if (end - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; v->t = JVal::Null; return true; } return fail(); }
        return number(v);
    }
    bool fail() { ok = false; return false; }
    bool string(std::string* s) {
        if (*p != '"') return fail();
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': *s += '\n'; break;
                    case 'r': *s += '\r'; break;
                    case 't': *s += '\t'; break;
                    case '"': *s += '"'; break;
                    case '\\': *s += '\\'; break;
                    case '/': *s += '/'; break;
                    case 'u': {
                        if (end - p < 4) return fail();
                        char hex[5] = {p[0], p[1], p[2], p[3], 0};
                        unsigned cp = static_cast<unsigned>(std::strtoul(hex, nullptr, 16));
                        p += 4;
                        if (cp < 0x80) *s += static_cast<char>(cp);
                        else if (cp < 0x800) { *s += static_cast<char>(0xC0 | (cp >> 6)); *s += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else { *s += static_cast<char>(0xE0 | (cp >> 12)); *s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); *s += static_cast<char>(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: *s += e; break;
                }
            } else *s += c;
        }
        if (p >= end) return fail();
        ++p;   // closing quote
        return true;
    }
    bool number(JVal* v) {
        const char* s = p;
        while (p < end && (std::isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) ++p;
        if (p == s) return fail();
        v->t = JVal::Num;
        v->num = std::strtod(std::string(s, p).c_str(), nullptr);
        return true;
    }
    bool boolean(JVal* v) {
        if (end - p >= 4 && !std::strncmp(p, "true", 4)) { p += 4; v->t = JVal::Bool; v->b = true; return true; }
        if (end - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; v->t = JVal::Bool; v->b = false; return true; }
        return fail();
    }
    bool array(JVal* v) {
        v->t = JVal::Arr;
        ++p; ws();
        if (p < end && *p == ']') { ++p; return true; }
        for (;;) {
            JVal e;
            if (!parse(&e)) return false;
            v->arr.push_back(std::move(e));
            ws();
            if (p >= end) return fail();
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return fail();
        }
    }
    bool object(JVal* v) {
        v->t = JVal::Obj;
        ++p; ws();
        if (p < end && *p == '}') { ++p; return true; }
        for (;;) {
            ws();
            std::string key;
            if (p >= end || *p != '"' || !string(&key)) return fail();
            ws();
            if (p >= end || *p != ':') return fail();
            ++p;
            JVal val;
            if (!parse(&val)) return false;
            v->obj.emplace_back(std::move(key), std::move(val));
            ws();
            if (p >= end) return fail();
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return fail();
        }
    }
};

inline bool parse_root(const std::string& json, JVal* out) {
    Parser pr{json.data(), json.data() + json.size()};
    return pr.parse(out) && pr.ok;
}

}  // namespace nxrt::cheats::json
