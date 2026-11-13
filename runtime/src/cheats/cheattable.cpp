// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Saved cheat table + layout (cheattable.h). A tiny self-contained JSON reader/writer: enough for the objects this module emits and for
// hand edits, no third-party dependency.
#include "cheattable.h"
#include "json_lite.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nxrt::cheats::table {
namespace {

// ---- JSON writer ----
void esc(std::string& o, const std::string& s) {
    o += '"';
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    o += '"';
}
std::string u64(uint64_t v) { char b[24]; std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(v)); return b; }
std::string i64(int64_t v) { char b[24]; std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v)); return b; }

// ---- JSON reader: json_lite.h ----
using json::JVal;
using json::parse_root;

}  // namespace

std::string dump_table(const std::vector<TableEntry>& entries) { return dump_table(entries, Extras()); }

std::string dump_table(const std::vector<TableEntry>& entries, const Extras& extras) {
    std::string o = "{\n  \"format\": 1,\n  \"entries\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const TableEntry& e = entries[i];
        o += "    {";
        o += "\"description\": "; esc(o, e.description);
        o += ", \"type\": " + std::to_string(e.type);
        o += ", \"value\": "; esc(o, e.value);
        o += ", \"freeze\": " + std::string(e.freeze ? "true" : "false");
        o += ", \"freeze_mode\": " + std::to_string(e.freeze_mode);
        o += ", \"hotkey\": " + std::to_string(e.hotkey);
        o += ", \"group\": "; esc(o, e.group);
        if (e.is_pointer) {
            o += ", \"pointer\": {\"module\": "; esc(o, e.path.module);
            o += ", \"mem_type\": " + std::to_string(e.path.mem_type);
            o += ", \"base_offset\": " + u64(e.path.base_offset);
            o += ", \"offsets\": [";
            for (size_t k = 0; k < e.path.offsets.size(); ++k) { if (k) o += ", "; o += u64(e.path.offsets[k]); }
            o += "]}";
        } else {
            o += ", \"address\": " + u64(e.address);
        }
        if (!e.signature.empty()) {
            o += ", \"signature\": "; esc(o, e.signature);
            o += ", \"sig_offset\": " + i64(e.sig_offset);
        }
        if (!e.hotkey_name.empty()) { o += ", \"hotkey_name\": "; esc(o, e.hotkey_name); }
        if (e.hotkey_action) o += ", \"hotkey_action\": " + std::to_string(e.hotkey_action);
        if (e.is_script) { o += ", \"script\": "; esc(o, e.script); }
        if (e.pinned) o += ", \"pinned\": true";
        o += "}";
        if (i + 1 < entries.size()) o += ",";
        o += "\n";
    }
    o += "  ]";
    if (!extras.labels.empty()) {
        o += ",\n  \"labels\": [\n";
        for (size_t i = 0; i < extras.labels.size(); ++i) {
            o += "    {\"address\": " + u64(extras.labels[i].address) + ", \"name\": ";
            esc(o, extras.labels[i].name);
            o += i + 1 < extras.labels.size() ? "},\n" : "}\n";
        }
        o += "  ]";
    }
    if (!extras.lua.empty()) {
        o += ",\n  \"lua\": [\n";
        for (size_t i = 0; i < extras.lua.size(); ++i) {
            o += "    {\"name\": ";
            esc(o, extras.lua[i].name);
            o += ", \"run\": " + std::string(extras.lua[i].run ? "true" : "false") + ", \"source\": ";
            esc(o, extras.lua[i].source);
            o += i + 1 < extras.lua.size() ? "},\n" : "}\n";
        }
        o += "  ]";
    }
    o += "\n}\n";
    return o;
}

std::string dump_layout(const Layout& l) {
    std::string o = "{\n  \"format\": 1,\n";
    auto b = [&](const char* k, bool v) { o += std::string("  \"") + k + "\": " + (v ? "true" : "false") + ",\n"; };
    b("scanner", l.scanner); b("list", l.list); b("viewer", l.viewer); b("pointer", l.pointer);
    b("files", l.files); b("scripts", l.scripts); b("help", l.help);
    b("disasm", l.disasm); b("watch", l.watch); b("memmap", l.memmap);
    if (!l.open.empty()) {
        o += "  \"open\": {";
        bool first = true;
        for (const auto& kv : l.open) {
            if (!first) o += ", ";
            first = false;
            esc(o, kv.first);
            o += std::string(": ") + (kv.second ? "true" : "false");
        }
        o += "},\n";
    }
    if (!l.rect.empty()) {
        o += "  \"rect\": {";
        bool first = true;
        for (const auto& kv : l.rect) {
            if (kv.second.size() != 4) continue;
            if (!first) o += ", ";
            first = false;
            esc(o, kv.first);
            o += ": [" + std::to_string(kv.second[0]) + ", " + std::to_string(kv.second[1]) + ", " + std::to_string(kv.second[2]) + ", " +
                 std::to_string(kv.second[3]) + "]";
        }
        o += "},\n";
    }
    if (!l.ui.empty()) {
        o += "  \"ui\": {";
        bool first = true;
        for (const auto& kv : l.ui) {
            if (!first) o += ", ";
            first = false;
            esc(o, kv.first);
            o += ": ";
            esc(o, kv.second);
        }
        o += "},\n";
    }
    o += "  \"viewer_addr\": " + u64(l.viewer_addr) + "\n}\n";
    return o;
}

bool parse_table(const std::string& json, std::vector<TableEntry>* out) { return parse_table(json, out, nullptr); }

bool parse_table(const std::string& json, std::vector<TableEntry>* out, Extras* extras) {
    JVal root;
    if (!parse_root(json, &root) || root.t != JVal::Obj) return false;
    const JVal* arr = root.get("entries");
    if (!arr || arr->t != JVal::Arr) return false;
    out->clear();
    for (const JVal& e : arr->arr) {
        if (e.t != JVal::Obj) continue;
        TableEntry te;
        if (auto* v = e.get("description")) te.description = v->as_str();
        if (auto* v = e.get("type")) te.type = v->as_int(2);
        if (auto* v = e.get("value")) te.value = v->as_str();
        if (auto* v = e.get("freeze")) te.freeze = v->as_bool();
        if (auto* v = e.get("freeze_mode")) te.freeze_mode = v->as_int();
        if (auto* v = e.get("hotkey")) te.hotkey = v->as_int();
        if (auto* v = e.get("group")) te.group = v->as_str();
        if (auto* v = e.get("signature")) te.signature = v->as_str();
        if (auto* v = e.get("sig_offset")) te.sig_offset = v->as_i64();
        if (auto* v = e.get("hotkey_name")) te.hotkey_name = v->as_str();
        if (auto* v = e.get("hotkey_action")) te.hotkey_action = v->as_int();
        if (auto* v = e.get("script")) { te.is_script = true; te.script = v->as_str(); }
        if (auto* v = e.get("pinned")) te.pinned = v->as_bool();
        if (const JVal* p = e.get("pointer"); p && p->t == JVal::Obj) {
            te.is_pointer = true;
            if (auto* v = p->get("module")) te.path.module = v->as_str();
            if (auto* v = p->get("mem_type")) te.path.mem_type = static_cast<unsigned>(v->as_u64());
            if (auto* v = p->get("base_offset")) te.path.base_offset = v->as_u64();
            if (const JVal* off = p->get("offsets"); off && off->t == JVal::Arr)
                for (const JVal& o : off->arr) te.path.offsets.push_back(o.as_u64());
        } else if (auto* v = e.get("address")) {
            te.address = v->as_u64();
        }
        out->push_back(std::move(te));
    }
    if (extras) {
        *extras = Extras();
        if (const JVal* lab = root.get("labels"); lab && lab->t == JVal::Arr)
            for (const JVal& l : lab->arr) {
                if (l.t != JVal::Obj) continue;
                Label x;
                if (auto* v = l.get("address")) x.address = v->as_u64();
                if (auto* v = l.get("name")) x.name = v->as_str();
                extras->labels.push_back(x);
            }
        if (const JVal* lua = root.get("lua"); lua && lua->t == JVal::Arr)
            for (const JVal& l : lua->arr) {
                if (l.t != JVal::Obj) continue;
                LuaScript x;
                if (auto* v = l.get("name")) x.name = v->as_str();
                if (auto* v = l.get("source")) x.source = v->as_str();
                if (auto* v = l.get("run")) x.run = v->as_bool();
                extras->lua.push_back(x);
            }
    }
    return true;
}

bool parse_layout(const std::string& json, Layout* out) {
    JVal root;
    if (!parse_root(json, &root) || root.t != JVal::Obj) return false;
    auto b = [&](const char* k, bool d) { auto* v = root.get(k); return v ? v->as_bool(d) : d; };
    out->scanner = b("scanner", out->scanner);
    out->list = b("list", out->list);
    out->viewer = b("viewer", out->viewer);
    out->pointer = b("pointer", out->pointer);
    out->files = b("files", out->files);
    out->scripts = b("scripts", out->scripts);
    out->help = b("help", out->help);
    out->disasm = b("disasm", out->disasm);
    out->watch = b("watch", out->watch);
    out->memmap = b("memmap", out->memmap);
    if (auto* v = root.get("viewer_addr")) out->viewer_addr = v->as_u64();
    if (const JVal* o = root.get("open"); o && o->t == JVal::Obj)
        for (const auto& kv : o->obj) out->open[kv.first] = kv.second.as_bool();
    if (const JVal* r = root.get("rect"); r && r->t == JVal::Obj)
        for (const auto& kv : r->obj)
            if (kv.second.t == JVal::Arr && kv.second.arr.size() == 4)
                for (const JVal& v : kv.second.arr) out->rect[kv.first].push_back(v.as_int());
    if (const JVal* u = root.get("ui"); u && u->t == JVal::Obj)
        for (const auto& kv : u->obj) out->ui[kv.first] = kv.second.as_str();
    return true;
}

}  // namespace nxrt::cheats::table
