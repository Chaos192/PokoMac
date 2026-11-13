// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin <intrin.h>: the MSVC intrinsics the runtime uses that have a meaning on arm64.
#pragma once
#include <cstdint>
#if defined(__aarch64__)
// The virtual counter stands in for the TSC (fixed frequency: CNTFRQ_EL0, 24 MHz on Apple silicon).
static inline uint64_t __rdtsc() { uint64_t v; __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline uint64_t nxwin_counter_hz() { uint64_t v; __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#else
#include <x86intrin.h>
#endif
#include "nxwin/sync.h"
