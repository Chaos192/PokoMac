// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Internal: host stack capture without allocation (safe to call while another thread is suspended).
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

// The program counter / stack pointer / frame pointer of a CONTEXT (x64 or arm64 layout).
#if defined(__aarch64__) || defined(_M_ARM64)
#define NXRT_CTX_PC(c) ((c).Pc)
#define NXRT_CTX_SP(c) ((c).Sp)
#define NXRT_CTX_FP(c) ((c).Fp)
#else
#define NXRT_CTX_PC(c) ((c).Rip)
#define NXRT_CTX_SP(c) ((c).Rsp)
#define NXRT_CTX_FP(c) ((c).Rbp)
#endif

namespace nxrt::detail {

// Walks the host stack described by *ctx with RtlLookupFunctionEntry/RtlVirtualUnwind. Writes up to `max` return
// addresses (the first is ctx->Rip). Does not modify *ctx. Never allocates.
size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max);

}  // namespace nxrt::detail
