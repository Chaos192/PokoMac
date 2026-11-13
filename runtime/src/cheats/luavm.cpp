// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Lua scripts for the cheats overlay (luavm.h).
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include "luavm.h"

// Lua is compiled as C++ (cmake/lua_deps.cmake), so its errors are C++ exceptions that unwind these functions' locals properly (a longjmp
// over C++ frames would skip their destructors).
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <type_traits>

#include "aob.h"

namespace nxrt::cheats::lua {

struct Vm::Impl {
    struct Hotkey { std::string name; int ref = LUA_NOREF; bool was_down = true; };   // was_down: no fire for a key already down at bind time
    struct Script {
        uint32_t id = 0;
        std::string name;
        int env = LUA_NOREF;
        std::vector<int> frame_fns;
        std::vector<Hotkey> hotkeys;
        bool running = false;
        std::string error;
        uint64_t calls = 0;
    };
    Host* host = nullptr;
    lua_State* L = nullptr;
    mutable std::mutex mu;
    std::map<uint32_t, Script> scripts;
    uint32_t next_id = 1;
    Script* cur = nullptr;                 // the script whose code is running (for onframe / hotkey registration)
    size_t mem_used = 0;
    uint64_t budget_left = 0;
    int console_env = LUA_NOREF;
    std::string eval_out;                  // what print wrote during eval()
    bool in_eval = false;

    static Impl* of(lua_State* L) {
        Impl* p = nullptr;
        lua_getallocf(L, reinterpret_cast<void**>(&p));
        return p;
    }
    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
        Impl* self = static_cast<Impl*>(ud);
        const size_t old = ptr ? osize : 0;
        if (nsize == 0) {
            self->mem_used -= old;
            std::free(ptr);
            return nullptr;
        }
        if (self->mem_used - old + nsize > kMemoryLimit) return nullptr;     // Lua raises "not enough memory"
        void* n = std::realloc(ptr, nsize);
        if (n) self->mem_used = self->mem_used - old + nsize;
        return n;
    }
    static void count_hook(lua_State* L, lua_Debug*) {
        Impl* self = of(L);
        if (self->budget_left <= 10000) {
            self->budget_left = 0;
            luaL_error(L, "the script took too long (more than %d million instructions in one call): stopped", static_cast<int>(kInstructionBudget / 1000000));
        }
        self->budget_left -= 10000;
    }

    void out(const std::string& line) {
        if (in_eval) eval_out += line + "\n";
        if (host) host->console((cur && !in_eval ? "[" + cur->name + "] " : std::string()) + line);
    }

    // ---- the nx library ----
    static uint64_t arg_addr(lua_State* L, int i) { return static_cast<uint64_t>(luaL_checkinteger(L, i)); }
    template <class T> static int rd(lua_State* L) {
        T v{};
        if (!of(L)->host->mem().read(arg_addr(L, 1), &v, sizeof v)) { lua_pushnil(L); return 1; }
        if constexpr (std::is_floating_point_v<T>) lua_pushnumber(L, static_cast<lua_Number>(v));
        else lua_pushinteger(L, static_cast<lua_Integer>(v));
        return 1;
    }
    template <class T> static int wr(lua_State* L) {
        T v;
        if constexpr (std::is_floating_point_v<T>) v = static_cast<T>(luaL_checknumber(L, 2));
        else v = static_cast<T>(luaL_checkinteger(L, 2));
        lua_pushboolean(L, of(L)->host->write(arg_addr(L, 1), &v, sizeof v));
        return 1;
    }
    static int readbytes(lua_State* L) {
        const lua_Integer n = luaL_checkinteger(L, 2);
        if (n < 0 || n > (1 << 20)) return luaL_error(L, "readbytes: 0..1048576 bytes");
        std::string b(static_cast<size_t>(n), '\0');
        if (!of(L)->host->mem().read(arg_addr(L, 1), b.data(), b.size())) { lua_pushnil(L); return 1; }
        lua_pushlstring(L, b.data(), b.size());
        return 1;
    }
    static int readstring(lua_State* L) {
        const lua_Integer mx = luaL_optinteger(L, 2, 256);
        std::string s;
        const uint64_t a = arg_addr(L, 1);
        for (lua_Integer i = 0; i < mx && i < 65536; ++i) {
            char c = 0;
            if (!of(L)->host->mem().read(a + i, &c, 1) || c == 0) break;
            s += c;
        }
        lua_pushlstring(L, s.data(), s.size());
        return 1;
    }
    static int writebytes(lua_State* L) {
        size_t n = 0;
        const char* s = luaL_checklstring(L, 2, &n);
        lua_pushboolean(L, of(L)->host->write(arg_addr(L, 1), s, n));
        return 1;
    }
    static int module(lua_State* L) {
        uint64_t lo = 0, hi = 0;
        if (!of(L)->host->module_range(luaL_checkstring(L, 1), &lo, &hi)) { lua_pushnil(L); return 1; }
        lua_pushinteger(L, static_cast<lua_Integer>(lo));
        lua_pushinteger(L, static_cast<lua_Integer>(hi - lo));
        return 2;
    }
    static int ptr(lua_State* L) {
        uint64_t p = arg_addr(L, 1);
        const int n = lua_gettop(L);
        for (int i = 2; i <= n; ++i) {
            uint64_t v = 0;
            if (!of(L)->host->mem().read(p, &v, 8)) { lua_pushnil(L); return 1; }
            p = v + static_cast<uint64_t>(luaL_checkinteger(L, i));
        }
        lua_pushinteger(L, static_cast<lua_Integer>(p));
        return 1;
    }
    static int scan(lua_State* L, bool all) {
        AobPattern pat;
        std::string err;
        if (!aob_parse(luaL_checkstring(L, 1), &pat, &err)) return luaL_error(L, "aobscan: %s", err.c_str());
        RegionFilter f;
        f.main = f.heap = f.alias = f.code = f.other = true;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            uint64_t lo = 0, hi = 0;
            if (!of(L)->host->module_range(luaL_checkstring(L, 2), &lo, &hi)) return luaL_error(L, "aobscan: no module named %s", lua_tostring(L, 2));
            f.lo = lo;
            f.hi = hi;
        }
        auto hits = aob_scan(of(L)->host->mem(), pat, f, all ? 100000 : 1);
        if (!all) {
            if (hits.empty()) lua_pushnil(L);
            else lua_pushinteger(L, static_cast<lua_Integer>(hits[0].addr));
            return 1;
        }
        lua_createtable(L, static_cast<int>(hits.size()), 0);
        for (size_t i = 0; i < hits.size(); ++i) {
            lua_pushinteger(L, static_cast<lua_Integer>(hits[i].addr));
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }
    static int aobscan(lua_State* L) { return scan(L, false); }
    static int aobscanall(lua_State* L) { return scan(L, true); }
    static bool type_of(const char* s, ScanType* t) {
        static const struct { const char* n; ScanType t; } k[] = {{"u8", ScanType::U8}, {"u16", ScanType::U16}, {"u32", ScanType::U32}, {"u64", ScanType::U64},
                                                                   {"f32", ScanType::F32}, {"f64", ScanType::F64}, {"byte", ScanType::U8}, {"float", ScanType::F32},
                                                                   {"double", ScanType::F64}, {"i32", ScanType::U32}, {"i64", ScanType::U64}};
        for (const auto& e : k) if (std::strcmp(s, e.n) == 0) { *t = e.t; return true; }
        return false;
    }
    static int freeze(lua_State* L) {
        ScanType t;
        if (!type_of(luaL_checkstring(L, 2), &t)) return luaL_error(L, "freeze: the type is u8, u16, u32, u64, f32 or f64");
        uint64_t bits = 0;
        if (t == ScanType::F32) { float f = static_cast<float>(luaL_checknumber(L, 3)); std::memcpy(&bits, &f, 4); }
        else if (t == ScanType::F64) { double d = luaL_checknumber(L, 3); std::memcpy(&bits, &d, 8); }
        else bits = static_cast<uint64_t>(luaL_checkinteger(L, 3));
        lua_pushinteger(L, static_cast<lua_Integer>(of(L)->host->freeze(arg_addr(L, 1), t, bits)));
        return 1;
    }
    static int unfreeze(lua_State* L) { of(L)->host->unfreeze(static_cast<uint64_t>(luaL_checkinteger(L, 1))); return 0; }
    static int onframe(lua_State* L) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        Impl* self = of(L);
        if (!self->cur) return luaL_error(L, "onframe: only a script can register (not the console)");
        lua_pushvalue(L, 1);
        self->cur->frame_fns.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
        return 0;
    }
    static int hotkey(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        Impl* self = of(L);
        if (!self->cur) return luaL_error(L, "hotkey: only a script can register (not the console)");
        lua_pushvalue(L, 2);
        Hotkey h;
        h.name = name;
        h.ref = luaL_ref(L, LUA_REGISTRYINDEX);
        h.was_down = self->host->held(name);
        self->cur->hotkeys.push_back(h);
        return 0;
    }
    static int held(lua_State* L) { lua_pushboolean(L, of(L)->host->held(luaL_checkstring(L, 1))); return 1; }
    static int pause(lua_State* L) { of(L)->host->pause(lua_toboolean(L, 1) != 0); return 0; }
    static int advance(lua_State* L) { of(L)->host->advance(); return 0; }
    static int speed(lua_State* L) {
        const double set = lua_gettop(L) >= 1 ? luaL_checknumber(L, 1) : 0.0;
        lua_pushnumber(L, of(L)->host->speed(set));
        return 1;
    }
    static int frame(lua_State* L) { lua_pushinteger(L, static_cast<lua_Integer>(of(L)->host->frame())); return 1; }
    static int tick(lua_State* L) { lua_pushinteger(L, static_cast<lua_Integer>(of(L)->host->tick())); return 1; }
    static int hosttime(lua_State* L) { lua_pushnumber(L, of(L)->host->host_seconds()); return 1; }
    static bool safe_name(const char* n) {
        const size_t len = std::strlen(n);
        if (len == 0 || len > 64 || n[0] == '.') return false;
        for (size_t i = 0; i < len; ++i) {
            const char c = n[i];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
        }
        return std::strstr(n, "..") == nullptr;
    }
    static int readfile(lua_State* L) {
        const char* n = luaL_checkstring(L, 1);
        const std::string dir = of(L)->host->folder();
        if (dir.empty() || !safe_name(n)) return luaL_error(L, "readfile: a plain file name in the cheats folder (letters, digits, _ - .)");
        std::ifstream f(std::filesystem::u8path(dir + "/" + n), std::ios::binary);
        if (!f) { lua_pushnil(L); return 1; }
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (s.size() > (4u << 20)) return luaL_error(L, "readfile: larger than 4 MiB");
        lua_pushlstring(L, s.data(), s.size());
        return 1;
    }
    static int writefile(lua_State* L) {
        const char* n = luaL_checkstring(L, 1);
        size_t len = 0;
        const char* s = luaL_checklstring(L, 2, &len);
        const std::string dir = of(L)->host->folder();
        if (dir.empty() || !safe_name(n)) return luaL_error(L, "writefile: a plain file name in the cheats folder (letters, digits, _ - .)");
        if (len > (1u << 20)) return luaL_error(L, "writefile: at most 1 MiB");
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        std::ofstream f(std::filesystem::u8path(dir + "/" + n), std::ios::binary);
        if (f) f.write(s, static_cast<std::streamsize>(len));
        lua_pushboolean(L, static_cast<bool>(f));
        return 1;
    }
    static std::string joined(lua_State* L) {
        std::string line;
        const int n = lua_gettop(L);
        for (int i = 1; i <= n; ++i) {
            size_t len = 0;
            const char* s = luaL_tolstring(L, i, &len);
            if (i > 1) line += '\t';
            line.append(s, len);
            lua_pop(L, 1);
        }
        return line;
    }
    static int print(lua_State* L) { of(L)->out(joined(L)); return 0; }
    // load() of text only: a precompiled (binary) chunk could break the VM's invariants, so it is refused as it is for scripts
    static int load_text(lua_State* L) {
        size_t n = 0;
        const char* s = luaL_checklstring(L, 1, &n);
        const char* name = luaL_optstring(L, 2, "=(load)");
        if (luaL_loadbufferx(L, s, n, name, "t") != LUA_OK) {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        if (!lua_isnoneornil(L, 4)) {                 // load(chunk, name, mode, env)
            lua_pushvalue(L, 4);
            if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
        }
        return 1;
    }

    void open() {
        L = lua_newstate(&Impl::alloc, this);
        // the libraries a sandboxed script may use (io, os, package and debug are not even built)
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
        luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(L, 6);
        // no loading code from files
        for (const char* n : {"dofile", "loadfile"}) {
            lua_pushnil(L);
            lua_setglobal(L, n);
        }
        lua_pushcfunction(L, &Impl::print);
        lua_setglobal(L, "print");
        lua_pushcfunction(L, &Impl::load_text);
        lua_setglobal(L, "load");
        lua_getglobal(L, "string");                  // no string.dump: nothing makes binary chunks
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
        lua_pop(L, 1);
        static const luaL_Reg nx[] = {
            {"read8", &rd<uint8_t>}, {"read16", &rd<uint16_t>}, {"read32", &rd<uint32_t>}, {"read64", &rd<uint64_t>},
            {"readf", &rd<float>}, {"readd", &rd<double>}, {"readbytes", &readbytes}, {"readstring", &readstring},
            {"write8", &wr<uint8_t>}, {"write16", &wr<uint16_t>}, {"write32", &wr<uint32_t>}, {"write64", &wr<uint64_t>},
            {"writef", &wr<float>}, {"writed", &wr<double>}, {"writebytes", &writebytes},
            {"module", &module}, {"ptr", &ptr}, {"aobscan", &aobscan}, {"aobscanall", &aobscanall},
            {"freeze", &freeze}, {"unfreeze", &unfreeze}, {"onframe", &onframe}, {"hotkey", &hotkey}, {"held", &held},
            {"pause", &pause}, {"advance", &advance}, {"speed", &speed}, {"frame", &frame}, {"tick", &tick}, {"time", &hosttime},
            {"readfile", &readfile}, {"writefile", &writefile}, {"log", &print}, {nullptr, nullptr}};
        luaL_newlib(L, nx);
        lua_setglobal(L, "nx");
        lua_sethook(L, &Impl::count_hook, LUA_MASKCOUNT, 10000);
    }

    // A fresh environment whose missing names fall through to the globals (each script keeps its own variables).
    int new_env() {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushglobaltable(L);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        return luaL_ref(L, LUA_REGISTRYINDEX);
    }

    // Calls the function on top of the stack with no arguments under the instruction budget. False with *err on an error.
    bool pcall0(std::string* err) {
        budget_left = kInstructionBudget;
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    void drop(Script& s) {
        for (int r : s.frame_fns) luaL_unref(L, LUA_REGISTRYINDEX, r);
        for (auto& h : s.hotkeys) luaL_unref(L, LUA_REGISTRYINDEX, h.ref);
        s.frame_fns.clear();
        s.hotkeys.clear();
        if (s.env != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.env);
        s.env = LUA_NOREF;
        s.running = false;
    }
    void fail(Script& s, const std::string& e) {
        s.error = e;
        out("stopped: " + e);
        drop(s);
    }
};

Vm::Vm(Host* host) : impl_(std::make_unique<Impl>()) {
    impl_->host = host;
    impl_->open();
}

Vm::~Vm() {
    if (impl_->L) lua_close(impl_->L);
}

uint32_t Vm::load(const std::string& name, const std::string& source, std::string* err) {
    Impl& s = *impl_;
    if (err) err->clear();
    std::lock_guard<std::mutex> lk(s.mu);
    lua_State* L = s.L;
    if (luaL_loadbufferx(L, source.data(), source.size(), ("=" + name).c_str(), "t") != LUA_OK) {   // text only: no precompiled chunks
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
        lua_pop(L, 1);
        return 0;
    }
    Impl::Script sc;
    sc.id = s.next_id++;
    sc.name = name;
    sc.env = s.new_env();
    lua_rawgeti(L, LUA_REGISTRYINDEX, sc.env);
    if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);            // the chunk's _ENV
    auto it = s.scripts.emplace(sc.id, sc).first;
    Impl::Script& live = it->second;
    live.running = true;
    s.cur = &live;
    std::string e;
    const bool ok = s.pcall0(&e);
    s.cur = nullptr;
    if (!ok) {
        s.fail(live, e);
        if (err) *err = e;
        return live.id;     // kept in the list with its error, so the overlay shows why
    }
    return live.id;
}

std::string Vm::eval(const std::string& line) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    lua_State* L = s.L;
    if (s.console_env == LUA_NOREF) s.console_env = s.new_env();
    s.eval_out.clear();
    s.in_eval = true;
    // an expression prints its value ("nx.read32(0x...)"), a statement just runs
    std::string src = "return " + line;
    if (luaL_loadbufferx(L, src.data(), src.size(), "=console", "t") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbufferx(L, line.data(), line.size(), "=console", "t") != LUA_OK) {
            std::string e = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
            lua_pop(L, 1);
            s.in_eval = false;
            return e;
        }
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.console_env);
    if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
    s.budget_left = kInstructionBudget;
    const int top = lua_gettop(L) - 1;
    std::string res;
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        res = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_settop(L, top);
    } else {
        const int n = lua_gettop(L) - top;
        for (int i = 1; i <= n; ++i) {
            size_t len = 0;
            const char* t = luaL_tolstring(L, top + i, &len);
            if (i > 1) res += '\t';
            res.append(t, len);
            lua_pop(L, 1);
        }
        lua_settop(L, top);
    }
    s.in_eval = false;
    std::string out = s.eval_out + res;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (!res.empty() && s.host) s.host->console(res);
    return out;
}

void Vm::stop(uint32_t id) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.scripts.find(id);
    if (it == s.scripts.end()) return;
    s.drop(it->second);
    s.scripts.erase(it);
}

void Vm::stop_all() {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    for (auto& kv : s.scripts) s.drop(kv.second);
    s.scripts.clear();
}

void Vm::on_frame(bool frame) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    lua_State* L = s.L;
    for (auto& kv : s.scripts) {
        Impl::Script& sc = kv.second;
        if (!sc.running) continue;
        s.cur = &sc;
        std::string e;
        bool ok = true;
        for (auto& h : sc.hotkeys) {
            const bool down = s.host->held(h.name);
            const bool fire = down && !h.was_down;
            h.was_down = down;
            if (!fire) continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
            ++sc.calls;
            if (!(ok = s.pcall0(&e))) break;
        }
        if (ok && frame) {
            const std::vector<int> fns = sc.frame_fns;       // a callback may register more
            for (int r : fns) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, r);
                ++sc.calls;
                if (!(ok = s.pcall0(&e))) break;
            }
        }
        s.cur = nullptr;
        if (!ok) s.fail(sc, e);
    }
}

std::vector<ScriptInfo> Vm::scripts() const {
    const Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<ScriptInfo> out;
    for (const auto& kv : s.scripts) {
        const Impl::Script& sc = kv.second;
        out.push_back({sc.id, sc.name, sc.running, sc.error, sc.frame_fns.size(), sc.hotkeys.size(), sc.calls});
    }
    return out;
}

}  // namespace nxrt::cheats::lua
