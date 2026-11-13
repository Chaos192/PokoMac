// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Chaos192
// nxwin: MSVC CRT names, the registry (always empty), the console, language ids, and `const char*` overloads of the W path functions
// (std::filesystem::path::c_str() is a UTF-8 `const char*` here where it is `const wchar_t*` on Windows).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nxwin/file.h"
#include "nxwin/msvcrt.h"

namespace nxwin {
std::string narrow(const wchar_t* w);
std::string fix_path(const char* p);
}  // namespace nxwin

// ---- the registry: there is none ----
#define HKEY_CLASSES_ROOT (reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x80000000)))
#define HKEY_CURRENT_USER (reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x80000001)))
#define HKEY_LOCAL_MACHINE (reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x80000002)))
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_REG_DWORD 0x00000010
#define RRF_RT_ANY 0x0000ffff
#define KEY_READ 0x20019
#define KEY_QUERY_VALUE 0x0001
#define KEY_WOW64_64KEY 0x0100
#define REG_SZ 1
#define REG_DWORD 4
typedef LONG LSTATUS;
inline LSTATUS RegGetValueA(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
inline LSTATUS RegGetValueW(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
inline LSTATUS RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, HKEY* out) { if (out) *out = nullptr; return ERROR_FILE_NOT_FOUND; }
inline LSTATUS RegOpenKeyExW(HKEY, LPCWSTR, DWORD, DWORD, HKEY* out) { if (out) *out = nullptr; return ERROR_FILE_NOT_FOUND; }
inline LSTATUS RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
inline LSTATUS RegQueryValueExW(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
inline LSTATUS RegCloseKey(HKEY) { return ERROR_SUCCESS; }

// ---- the console: a process has its stdio from the terminal or launchd; there is no console to attach to ----
#define ATTACH_PARENT_PROCESS (static_cast<DWORD>(-1))
inline BOOL AttachConsole(DWORD) { return FALSE; }
inline BOOL AllocConsole() { return FALSE; }
inline BOOL FreeConsole() { return TRUE; }
inline HWND GetConsoleWindow() { return nullptr; }
inline BOOL SetConsoleOutputCP(UINT) { return TRUE; }
inline BOOL SetConsoleTitleA(LPCSTR) { return TRUE; }
typedef BOOL(WINAPI* PHANDLER_ROUTINE)(DWORD);
#define CTRL_C_EVENT 0
#define CTRL_BREAK_EVENT 1
#define CTRL_CLOSE_EVENT 2
BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE h, BOOL add);

// ---- GDI object counts (GetGuiResources) ----
#define GR_GDIOBJECTS 0
#define GR_USEROBJECTS 1
#define GR_GDIOBJECTS_PEAK 2
#define GR_USEROBJECTS_PEAK 4

// ---- language ids (winnt.h) ----
#define LANG_NEUTRAL 0x00
#define LANG_ARABIC 0x01
#define LANG_CHINESE 0x04
#define LANG_CHINESE_SIMPLIFIED 0x04
#define LANG_CHINESE_TRADITIONAL 0x7c04
#define LANG_DANISH 0x06
#define LANG_DUTCH 0x13
#define LANG_ENGLISH 0x09
#define LANG_FINNISH 0x0b
#define LANG_FRENCH 0x0c
#define LANG_GERMAN 0x07
#define LANG_HINDI 0x39
#define LANG_ITALIAN 0x10
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_NORWEGIAN 0x14
#define LANG_POLISH 0x15
#define LANG_PORTUGUESE 0x16
#define LANG_RUSSIAN 0x19
#define LANG_SPANISH 0x0a
#define LANG_SWEDISH 0x1d
#define LANG_TURKISH 0x1f
#define SUBLANG_NEUTRAL 0x00
#define SUBLANG_DEFAULT 0x01
#define SUBLANG_CHINESE_TRADITIONAL 0x01
#define SUBLANG_CHINESE_SIMPLIFIED 0x02
#define SUBLANG_CHINESE_HONGKONG 0x03
#define SUBLANG_CHINESE_SINGAPORE 0x04
#define SUBLANG_CHINESE_MACAU 0x05
#define SUBLANG_ENGLISH_US 0x01
#define SUBLANG_ENGLISH_UK 0x02
#define SUBLANG_ENGLISH_AUS 0x03
#define SUBLANG_ENGLISH_CAN 0x04
#define SUBLANG_FRENCH 0x01
#define SUBLANG_FRENCH_CANADIAN 0x03
#define SUBLANG_PORTUGUESE_BRAZILIAN 0x01
#define SUBLANG_PORTUGUESE 0x02
#define SUBLANG_SPANISH 0x01
#define SUBLANG_SPANISH_MEXICAN 0x02
#define SUBLANG_SPANISH_MODERN 0x03
#define SUBLANG_SPANISH_LATIN_AMERICA 0x580a

// ---- the W path functions with UTF-8 paths ----
inline HANDLE CreateFileW(const char* p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) { return CreateFileA(p, a, s, sa, d, f, t); }
inline DWORD GetFileAttributesW(const char* p) { return GetFileAttributesA(p); }
inline BOOL GetFileAttributesExW(const char* p, GET_FILEEX_INFO_LEVELS l, LPVOID o) { return GetFileAttributesExA(p, l, o); }
inline BOOL SetFileAttributesW(const char* p, DWORD a) { return SetFileAttributesA(p, a); }
inline BOOL DeleteFileW(const char* p) { return DeleteFileA(p); }
inline BOOL MoveFileExW(const char* a, const char* b, DWORD f) { return MoveFileExA(a, b, f); }
inline BOOL MoveFileW(const char* a, const char* b) { return MoveFileA(a, b); }
inline BOOL CopyFileW(const char* a, const char* b, BOOL f) { return CopyFileA(a, b, f); }
inline BOOL CreateDirectoryW(const char* p, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryA(p, sa); }
inline BOOL RemoveDirectoryW(const char* p) { return RemoveDirectoryA(p); }
inline BOOL SetCurrentDirectoryW(const char* p) { return SetCurrentDirectoryA(p); }
HANDLE FindFirstFileW(const char* p, LPWIN32_FIND_DATAW d);
BOOL ReplaceFileW(const char* replaced, const char* replacement, const char* backup, DWORD flags, LPVOID, LPVOID);
