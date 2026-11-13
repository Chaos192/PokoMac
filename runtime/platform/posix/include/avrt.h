// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Chaos192
// nxwin <avrt.h>: MMCSS task registration raises the calling thread to the user-interactive QoS class on macOS.
#pragma once
#include "windows.h"
HANDLE AvSetMmThreadCharacteristicsA(LPCSTR task, LPDWORD index);
HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR task, LPDWORD index);
BOOL AvRevertMmThreadCharacteristics(HANDLE h);
typedef enum _AVRT_PRIORITY { AVRT_PRIORITY_VERYLOW = -2, AVRT_PRIORITY_LOW, AVRT_PRIORITY_NORMAL, AVRT_PRIORITY_HIGH, AVRT_PRIORITY_CRITICAL } AVRT_PRIORITY;
BOOL AvSetMmThreadPriority(HANDLE h, AVRT_PRIORITY p);
