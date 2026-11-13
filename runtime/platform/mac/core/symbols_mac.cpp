// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// macOS host-code symbolisation (src/diag/symbols.cpp reads MSVC linker maps on Windows): dladdr names the image and the nearest symbol,
// demangled. There is no map to load; symbols_load_map reports 0 and lookups still work.
#include "nxrt/symbols.h"

#include <cxxabi.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nxrt {

size_t symbols_load_map(const char*) { return 0; }
std::string symbols_map_path() { return {}; }
size_t symbols_load_map_text(const std::string&) { return 0; }
size_t symbols_count() { return 0; }

SymbolInfo symbols_lookup(uint64_t host_addr) {
    SymbolInfo si;
    Dl_info di;
    if (!dladdr(reinterpret_cast<void*>(host_addr), &di) || !di.dli_fbase) {
        si.module = "?";
        return si;
    }
    const char* base = di.dli_fname ? std::strrchr(di.dli_fname, '/') : nullptr;
    si.module = base ? base + 1 : (di.dli_fname ? di.dli_fname : "?");
    si.offset = host_addr - reinterpret_cast<uint64_t>(di.dli_fbase);
    if (di.dli_sname && di.dli_saddr) {
        int st = 0;
        char* d = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st);
        si.symbol = st == 0 && d ? d : di.dli_sname;
        std::free(d);
        si.sym_offset = host_addr - reinterpret_cast<uint64_t>(di.dli_saddr);
    }
    return si;
}

std::string symbols_describe(uint64_t host_addr) {
    const SymbolInfo s = symbols_lookup(host_addr);
    char b[96];
    if (s.module == "?") {
        std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(host_addr));
        return b;
    }
    std::snprintf(b, sizeof b, "%s+0x%llx", s.module.c_str(), static_cast<unsigned long long>(s.offset));
    std::string out = b;
    if (!s.symbol.empty()) {
        std::snprintf(b, sizeof b, "+0x%llx)", static_cast<unsigned long long>(s.sym_offset));
        out += " (" + s.symbol + b;
    }
    return out;
}

}  // namespace nxrt
