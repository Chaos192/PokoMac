// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin <bcrypt.h>: BCryptGenRandom over arc4random_buf.
#pragma once
#include "windows.h"
#include <cstdlib>
typedef void* BCRYPT_ALG_HANDLE;
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#define STATUS_SUCCESS ((NTSTATUS)0)
#define BCRYPT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
inline NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE, unsigned char* buf, ULONG n, ULONG) { arc4random_buf(buf, n); return 0; }
