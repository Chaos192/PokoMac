// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: the subset of the Win32 API the runtime uses, implemented over POSIX and Mach (macOS port).
// This directory is on the include path only for non-Windows builds, so `#include <windows.h>` and friends resolve here and
// the runtime's sources compile unchanged. Types keep their Win32 sizes (DWORD and LONG are 32-bit) so structure layouts match;
// GetLastError returns `unsigned long` so the many "%lu" log arguments stay well-formed on LP64.
#pragma once
#ifndef __cplusplus
#error "nxwin is C++ only"
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#define NXWIN 1

#ifndef WINAPI
#define WINAPI
#endif
#define CALLBACK
#define APIENTRY
#define WINBASEAPI
#define NTAPI
#define __stdcall
#define __cdecl
#define __fastcall
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#define DECLSPEC_NOINLINE __attribute__((noinline))
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define _Inout_
#define _In_
#define _Out_
#define _In_opt_
#define _Out_opt_

typedef int BOOL;
typedef uint8_t BOOLEAN;
typedef uint8_t BYTE;
typedef uint8_t UCHAR;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef uint16_t WORD;
typedef uint16_t USHORT;
typedef int16_t SHORT;
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef int32_t INT;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORD64;
typedef uint64_t ULONG64;
typedef int64_t LONG64;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef uint64_t UINT64;
typedef int64_t INT64;
typedef uint16_t UINT16;
typedef uint8_t UINT8;
typedef uintptr_t ULONG_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t INT_PTR;
typedef size_t SIZE_T;
typedef intptr_t SSIZE_T;
typedef int32_t HRESULT;
typedef int32_t NTSTATUS;
typedef float FLOAT;
typedef void VOID;
typedef void* PVOID;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef wchar_t* PWSTR;
typedef const wchar_t* PCWSTR;
typedef DWORD* LPDWORD;
typedef DWORD* PDWORD;
typedef LONG* PLONG;
typedef BOOL* LPBOOL;
typedef BYTE* LPBYTE;
typedef BYTE* PBYTE;
typedef ULONG* PULONG;
typedef SIZE_T* PSIZE_T;
typedef ULONG_PTR* PULONG_PTR;
typedef DWORD_PTR* PDWORD_PTR;
typedef ULONG64* PULONG64;
typedef UINT64* PUINT64;
typedef UCHAR* PUCHAR;
typedef USHORT* PUSHORT;
typedef WORD* PWORD;
typedef WORD* LPWORD;
typedef LONG64* PLONG64;
typedef LONGLONG* PLONGLONG;
typedef INT* PINT;
typedef UINT* PUINT;
typedef CHAR* PCHAR;
typedef const CHAR* PCSTR;
typedef CHAR* PSTR;

typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef HANDLE* LPHANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HWND;
typedef void* HMONITOR;
typedef void* HDC;
typedef void* HKEY;
typedef void* HICON;
typedef void* HGLOBAL;
typedef void* HLOCAL;
typedef void (*FARPROC)();
typedef intptr_t (*NEARPROC)();

#define TRUE 1
#define FALSE 0
#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)

#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME, *PFILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _GUID { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } GUID, IID, CLSID;
typedef const GUID& REFGUID;
typedef const IID& REFIID;

typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef struct _OVERLAPPED { ULONG_PTR Internal, InternalHigh; union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; }; HANDLE hEvent; } OVERLAPPED, *LPOVERLAPPED;

// ---- error codes (winerror.h values) ----
#define ERROR_SUCCESS 0L
#define NO_ERROR 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_ADDRESS 487L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_INVALID_DRIVE 15L
#define ERROR_GEN_FAILURE 31L
#define ERROR_SEM_TIMEOUT 121L
#define ERROR_INVALID_DATA 13L
#define ERROR_BAD_LENGTH 24L
#define ERROR_NETNAME_DELETED 64L
#define ERROR_INVALID_FLAGS 1004L
#define ERROR_OPERATION_ABORTED 995L
#define WAIT_IO_COMPLETION_ 192L
#define ERROR_NOT_SAME_DEVICE 17L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_NOT_READY 21L
#define ERROR_CRC 23L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_HANDLE_DISK_FULL 39L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_DISK_FULL 112L
#define ERROR_CALL_NOT_IMPLEMENTED 120L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_BUSY 170L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_DIRECTORY 267L
#define ERROR_NOT_OWNER 288L
#define ERROR_PARTIAL_COPY 299L
#define ERROR_IO_PENDING 997L
#define ERROR_NOACCESS 998L
#define ERROR_COMMITMENT_LIMIT 1455L
#define ERROR_TIMEOUT 1460L
#define ERROR_USER_MAPPED_FILE 1224L
#define ERROR_CANCELLED 1223L
#define ERROR_NOT_FOUND 1168L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#define ERROR_UNRECOGNIZED_VOLUME 1005L
#define ERROR_CANT_ACCESS_FILE 1920L
#define ERROR_BAD_EXE_FORMAT 193L
#define ERROR_DLL_INIT_FAILED 1114L
#define ERROR_BAD_PATHNAME 161L
#define ERROR_DIRECTORY_NOT_SUPPORTED 336L

namespace nxwin {
DWORD errno_to_win32(int e);
}

unsigned long GetLastError();
void SetLastError(DWORD e);
