// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin <psapi.h>: process memory counters from task_info.
#pragma once
#include "windows.h"
typedef struct _PROCESS_MEMORY_COUNTERS {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage,
        PagefileUsage, PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;
typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage,
        PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, *PPROCESS_MEMORY_COUNTERS_EX;
// WorkingSetSize = resident size, PagefileUsage = PrivateUsage = phys_footprint (what Activity Monitor calls Memory).
BOOL GetProcessMemoryInfo(HANDLE proc, PPROCESS_MEMORY_COUNTERS pmc, DWORD cb);
typedef struct _PERFORMANCE_INFORMATION {
    DWORD cb; SIZE_T CommitTotal, CommitLimit, CommitPeak, PhysicalTotal, PhysicalAvailable, SystemCache, KernelTotal, KernelPaged, KernelNonpaged,
        PageSize; DWORD HandleCount, ProcessCount, ThreadCount;
} PERFORMANCE_INFORMATION, *PPERFORMANCE_INFORMATION;
BOOL GetPerformanceInfo(PPERFORMANCE_INFORMATION pi, DWORD cb);
