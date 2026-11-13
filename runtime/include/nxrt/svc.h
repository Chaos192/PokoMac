// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SVC dispatch table. Calling convention of every handler (Switchbrew "SVC" page, AArch64 column): inputs in x0..x7, the result
// code in w0 (x0 upper half zero) and further outputs in x1.. exactly as documented per call. Handlers are plain functions on the
// guest context; a handler that blocks blocks the calling host thread (a guest thread is one host thread).
// Dispatch pattern from SuperMarioWonderRecomp runtime/src/kernel.cpp svc_impl (MIT); the unknown-SVC log follows smo-recomp's
// "unimplemented import" reporting. Names: src/kernel/svc_names.inc (Switchbrew, 0x01..0x91).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/abi.h"

namespace nxrt {

using SvcHandler = void (*)(::Ctx*);

constexpr uint32_t kSvcCount = 0x100;

void svc_init();                                           // registers every built-in handler (idempotent)
void svc_register(uint32_t number, SvcHandler fn);         // add or replace a handler (game-specific SVCs, tests)
SvcHandler svc_handler(uint32_t number);                   // nullptr when none
const char* svc_name(uint32_t number);                     // Switchbrew name or "?" for a number nobody documents
// Entry point of the lifted code's `svc #n` (guest_svc). `pc` is the guest address of the svc instruction when the caller knows it
// (0 otherwise; the log then names the lifted function through ctx->fn and x30).
void svc_dispatch(::Ctx* c, uint32_t number, uint64_t pc = 0);
// A function svc_dispatch calls before every SVC, on the calling thread (null: none). Play mode keeps its window answering with it while
// the game boots or loads and no frame boundary pumps the events; it must be cheap (it runs on every SVC of every guest thread).
void svc_set_busy_hook(void (*fn)());

// ---- unknown SVC bookkeeping --------------------------------------------------------------------------------------------------
// A number with no handler logs "UNKNOWN SVC 0x%02X (<name>) x0..x7 caller=..." (every call up to [svc] unknown_log_limit, default
// 100, per number; afterwards every 1000th), counts it, and returns kernel::kNotImplemented (0x4201, ResultNotImplemented) in w0.
struct SvcUnknownRecord {
    uint32_t number = 0;
    uint64_t count = 0;
    uint64_t first_args[8] = {};
    uint64_t first_lr = 0, first_fn = 0, first_pc = 0;
};
std::vector<SvcUnknownRecord> svc_unknown_records();
uint64_t svc_unknown_total();
uint64_t svc_call_count(uint32_t number);                  // calls dispatched to a handler or counted as unknown
void svc_reset_counters();                                 // tests
std::string svc_summary();                                 // "svc: N calls, M distinct, K unknown (0x.., ...)" for the end-of-run log

// ---- groups registered by svc_init (one source file each) ------------------------------------------------------------------------
void svc_register_memory_group();                          // svc_memory.cpp: heap, MapMemory, QueryMemory, shared/transfer memory
void svc_register_info_group();                            // svc_info.cpp: GetInfo, GetSystemInfo, resource limits, process/thread ids
void svc_register_thread_group();                          // svc_thread.cpp: threads, events, sync, time, handles, break
void svc_register_ipc_group();                             // svc_ipc.cpp: ConnectToNamedPort, SendSyncRequest*

// Current size set by svcSetHeapSize (0 before the first call); implemented by the memory group, read by the info group.
uint64_t svc_current_heap_size();
// The physical memory the process may use: what GetInfo(TotalMemorySize) reports and the ceiling svcSetHeapSize enforces together with the
// mapped module images and a fixed overhead (the kernel's resource limit). Default 0xCD500000 (a console application's pool);
// [memory] app_memory_mb overrides.
uint64_t svc_total_memory();

// Helper for handlers.
inline void svc_ret(::Ctx* c, uint32_t rc) { c->x[0] = rc; }

}  // namespace nxrt
