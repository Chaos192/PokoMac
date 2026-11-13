// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nvn_abi.h"

#include <atomic>
#include <cstring>

#include "nxrt/log.h"

namespace nxrt::nvn::abi {
namespace {
std::atomic<unsigned> g_mismatches{0};
}

bool register_checked(const char* name, HleFn fn, char ret, const char* args) {
    const int idx = entry_index(name);
    if (idx < 0) {
        NXLOG_E("nvn", "implementation of %s: not an NVN entry point of the table", name);
        g_mismatches.fetch_add(1);
        return false;
    }
    const Entry& e = entry_at(static_cast<size_t>(idx));
    // the generated table was read from a header whose typedefs are wrong for these two (they return a pointer)
    const bool wrong_void = e.ret == 'v' && ret == 'p' && (!std::strcmp(name, "nvnMemoryPoolBuilderGetMemory") || !std::strcmp(name, "nvnCommandBufferGetMemoryCallbackData"));
    if (e.args[0] != '?') {
        bool ok = classes_compatible(e.ret, ret) || wrong_void;
        const char* a = e.args;
        const char* b = args;
        while (ok && (*a || *b)) {
            // the table says 'S' for a struct by value and '?' is handled above; 'p' / 'l' compare equal
            if (!*a || !*b || !classes_compatible(*a, *b)) ok = false;
            else { ++a; ++b; }
        }
        if (!ok) {
            NXLOG_E("nvn", "implementation of %s has classes %c(%s) but the table says %c(%s)", name, ret, args, e.ret, e.args);
            g_mismatches.fetch_add(1);
            return false;
        }
    }
    return register_impl(name, fn);
}

unsigned mismatches() { return g_mismatches.load(); }
void reset_mismatches() { g_mismatches.store(0); }

}  // namespace nxrt::nvn::abi
