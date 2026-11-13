// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Chaos192
// nxwin: x86 SIMD headers on arm64 provide only _mm_pause (a spin-wait hint); SIMD code needs its own arm64 path.
#pragma once
#if defined(__aarch64__)
static inline void _mm_pause() { __asm__ __volatile__("yield"); }
#else
#include_next <immintrin.h>
#endif
