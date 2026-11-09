// SPDX-License-Identifier: GPL-3.0-or-later
// Pattern from smo-recomp runtime/core/hostrt.c crash_filter (RtlVirtualUnwind loop).
#include "stackwalk.h"

namespace nxrt::detail {

#if defined(_M_X64) || (defined(__x86_64__) && defined(_WIN32))
size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max) {
    CONTEXT c = *ctx;
    size_t n = 0;
    while (n < max && c.Rip) {
        frames[n++] = c.Rip;
        DWORD64 image_base = 0;
        RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!rf) {  // leaf function without unwind info: the return address is at [rsp]
            if (c.Rsp < 0x10000) break;
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            if (n > 1) break;   // only the first frame may be a leaf; anything else means we lost the chain
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler_data, &establisher, nullptr);
    }
    return n;
}
#else
// arm64 (macOS keeps frame pointers in all code): pc, then the lr of each frame record {fp, lr} on the fp chain. Every read goes through
// ReadProcessMemory on ourselves, so a broken chain ends the walk instead of faulting.
size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max) {
    size_t n = 0;
    if (n < max && ctx->Pc) frames[n++] = ctx->Pc;
    uint64_t fp = ctx->Fp;
    if (n < max && ctx->Lr) frames[n++] = ctx->Lr & 0x0000FFFFFFFFFFFFull;
    while (n < max && fp && !(fp & 0xF)) {
        uint64_t rec[2] = {0, 0};
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(fp), rec, sizeof rec, &got) || got != sizeof rec) break;
        const uint64_t lr = rec[1] & 0x0000FFFFFFFFFFFFull;
        if (!lr) break;
        if (n > 2 || lr != frames[n - 1]) frames[n++] = lr;
        if (rec[0] <= fp || rec[0] - fp > (64ull << 20)) break;   // frames go up the stack, and not by more than a stack's size
        fp = rec[0];
    }
    return n;
}
#endif

}  // namespace nxrt::detail
