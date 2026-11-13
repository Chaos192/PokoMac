// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin: the MSVC CRT names the runtime uses (_wfopen, _putenv, localtime_s, ...). Force-included into every C++ file of the runtime on
// macOS, as the MSVC CRT is visible everywhere on Windows; it defines no Win32 macros.
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nxwin {
std::string narrow(const wchar_t* w);
std::string fix_path(const char* p);
}  // namespace nxwin

// ---- CRT ----
inline FILE* _wfopen(const wchar_t* p, const wchar_t* m) { return std::fopen(nxwin::fix_path(nxwin::narrow(p).c_str()).c_str(), nxwin::narrow(m).c_str()); }
inline FILE* _wfopen(const char* p, const wchar_t* m) { return std::fopen(nxwin::fix_path(p).c_str(), nxwin::narrow(m).c_str()); }
inline int _wfopen_s(FILE** f, const wchar_t* p, const wchar_t* m) { *f = _wfopen(p, m); return *f ? 0 : errno; }
inline int fopen_s(FILE** f, const char* p, const char* m) { *f = std::fopen(p, m); return *f ? 0 : errno; }
inline int _wremove(const wchar_t* p) { return std::remove(nxwin::narrow(p).c_str()); }
inline int _putenv(const char* s) { return ::putenv(strdup(s)); }
inline int _putenv_s(const char* n, const char* v) { return ::setenv(n, v, 1); }
inline char* _getcwd(char* b, int n) { return ::getcwd(b, static_cast<size_t>(n)); }
inline int _chdir(const char* p) { return ::chdir(p); }
inline int _mkdir(const char* p) { return ::mkdir(p, 0755); }
inline unsigned long long _strtoui64(const char* s, char** e, int b) { return std::strtoull(s, e, b); }
inline long long _strtoi64(const char* s, char** e, int b) { return std::strtoll(s, e, b); }
inline unsigned long long _wcstoui64(const wchar_t* s, wchar_t** e, int b) { return std::wcstoull(s, e, b); }
inline long long _atoi64(const char* s) { return std::atoll(s); }
inline int _wtoi(const wchar_t* s) { return static_cast<int>(std::wcstol(s, nullptr, 10)); }
inline int localtime_s(struct tm* out, const time_t* t) { return ::localtime_r(t, out) ? 0 : errno; }
inline int gmtime_s(struct tm* out, const time_t* t) { return ::gmtime_r(t, out) ? 0 : errno; }
inline time_t _mkgmtime(struct tm* t) { return ::timegm(t); }
inline int _isatty(int fd) { return ::isatty(fd); }
#define _O_BINARY 0
#define _O_TEXT 0
inline int _setmode(int, int) { return 0; }
#define strcpy_s(d, n, s) std::snprintf((d), (n), "%s", (s))
#define strncpy_s(d, n, s, c) std::snprintf((d), (n), "%.*s", static_cast<int>(c), (s))
#define strcat_s(d, n, s) std::strncat((d), (s), (n) - std::strlen(d) - 1)
#define _TRUNCATE (static_cast<size_t>(-1))
#define vsprintf_s vsnprintf
#define _vsnprintf_s(b, n, c, f, a) vsnprintf((b), (n), (f), (a))
#define _snprintf_s(b, n, c, ...) snprintf((b), (n), __VA_ARGS__)
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define __FUNCSIG__ __PRETTY_FUNCTION__

