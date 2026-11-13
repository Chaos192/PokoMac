// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// macOS: claims the guest's address space when the runtime image loads, before the process has made many allocations of its own.
//
// The guest space on macOS is [0x71_0000_0000, 0x80_0000_0000) (include/nxrt/memory.h): the module images at their lifted addresses, the
// alias, heap, stack, remote and kernel regions. pkp is linked with SDK version 12.0, so the kernel lays the process out the classic way
// (no randomly slid data range) and the first large anonymous allocations go to 0x70_0000_0000 and up, first fit. This holds the whole
// window (nxwin vm_hold: inaccessible reservations that the runtime's fixed reservations and views later replace) so that nothing else lands
// in it later; host allocations then go to [0x70, 0x71) and above 0x80. Whatever was mapped there already is left alone; mem::init logs it.
#include <windows.h>

#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::mem {
namespace {

uint64_t g_missed = 0;

__attribute__((constructor(101))) void claim_guest_window() { g_missed = nxwin::vm_hold(kGuestWindowLo, kGuestWindowHi - kGuestWindowLo); }

}  // namespace

void mac_log_guest_window() {
    if (g_missed)
        NXLOG_W("mem", "guest window %llx..%llx: %llu MiB were already in use by the process (is pkp linked with -platform_version macos 12.0 12.0?)",
                static_cast<unsigned long long>(kGuestWindowLo), static_cast<unsigned long long>(kGuestWindowHi),
                static_cast<unsigned long long>(g_missed >> 20));
    else
        NXLOG_I("mem", "guest window %llx..%llx held", static_cast<unsigned long long>(kGuestWindowLo), static_cast<unsigned long long>(kGuestWindowHi));
    // Nothing below the window is the guest's (the shared cache, a kernel hole, the host's own allocations): svcQueryMemory says so, so the
    // SDK's random placement (MapAtRandomAddress) never picks an address there.
    vm_set(0x0008000000ull, kGuestWindowLo - 0x0008000000ull, kMemInaccessible, 0);
    vm_set(kHleBase, kHleSize, kMemInaccessible, 0);
}

}  // namespace nxrt::mem
