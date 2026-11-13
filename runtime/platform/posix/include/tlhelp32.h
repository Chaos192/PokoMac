// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin <tlhelp32.h>: thread snapshots of the calling process (task_threads); other snapshot kinds are empty.
#pragma once
#include "windows.h"
#define TH32CS_SNAPTHREAD 0x00000004
#define TH32CS_SNAPPROCESS 0x00000002
#define TH32CS_SNAPMODULE 0x00000008
typedef struct tagTHREADENTRY32 { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32, *LPTHREADENTRY32;
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL Thread32First(HANDLE snap, LPTHREADENTRY32 te);
BOOL Thread32Next(HANDLE snap, LPTHREADENTRY32 te);
