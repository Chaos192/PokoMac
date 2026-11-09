// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: strings and code pages, environment, modules (dlopen), time, processor information, message boxes, the shell.
#pragma once
#include "nxwin/handles.h"

// ---- code pages: CP_ACP and CP_UTF8 are both UTF-8; wchar_t is UTF-32 on macOS ----
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x8
#define WC_ERR_INVALID_CHARS 0x80
#define WC_NO_BEST_FIT_CHARS 0x400
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int n, LPWSTR dst, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int n, LPSTR dst, int cap, LPCSTR def, LPBOOL used_def);
inline int lstrlenW(LPCWSTR s) { return s ? static_cast<int>(wcslen(s)) : 0; }
inline int lstrlenA(LPCSTR s) { return s ? static_cast<int>(strlen(s)) : 0; }
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define sprintf_s snprintf
#define _strdup strdup
#define _wcsdup wcsdup
#define strtok_s strtok_r
#define _fileno fileno
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _access access
#define _getpid getpid
#define _unlink unlink
#define _byteswap_ushort __builtin_bswap16
#define _byteswap_ulong __builtin_bswap32
#define _byteswap_uint64 __builtin_bswap64
#define _rotl(x, n) __builtin_rotateleft32((x), (n))
#define _rotr(x, n) __builtin_rotateright32((x), (n))
#define _rotl64(x, n) __builtin_rotateleft64((x), (n))
#define _rotr64(x, n) __builtin_rotateright64((x), (n))
#define _alloca __builtin_alloca
#define _aligned_malloc(size, align) ::nxwin::aligned_malloc((size), (align))
#define _aligned_free(p) ::free(p)
namespace nxwin {
void* aligned_malloc(size_t size, size_t align);
}
inline unsigned char _BitScanForward(unsigned long* i, unsigned long m) { if (!m) return 0; *i = static_cast<unsigned long>(__builtin_ctzl(m)); return 1; }
inline unsigned char _BitScanReverse(unsigned long* i, unsigned long m) { if (!m) return 0; *i = 63ul - static_cast<unsigned long>(__builtin_clzl(m)); return 1; }
inline unsigned char _BitScanForward64(unsigned long* i, uint64_t m) { if (!m) return 0; *i = static_cast<unsigned long>(__builtin_ctzll(m)); return 1; }
inline unsigned char _BitScanReverse64(unsigned long* i, uint64_t m) { if (!m) return 0; *i = 63ul - static_cast<unsigned long>(__builtin_clzll(m)); return 1; }
#define __popcnt __builtin_popcount
#define __popcnt64 __builtin_popcountll

// ---- environment and command line ----
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD n);
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value);
DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD n);
LPSTR GetCommandLineA();
LPWSTR GetCommandLineW();
LPWSTR* CommandLineToArgvW(LPCWSTR cmd, int* argc);
HLOCAL LocalFree(HLOCAL p);
DWORD GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file);
UINT GetPrivateProfileIntA(LPCSTR sec, LPCSTR key, INT def, LPCSTR file);
BOOL WritePrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR value, LPCSTR file);

// ---- modules ----
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x1
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryW(LPCWSTR name);
HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
HMODULE LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
#define LoadLibrary LoadLibraryW
BOOL FreeLibrary(HMODULE m);
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out);
BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out);
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n);
DWORD GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD n);
BOOL SetDllDirectoryW(LPCWSTR dir);

// ---- time ----
BOOL QueryPerformanceCounter(LARGE_INTEGER* c);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* f);
ULONGLONG GetTickCount64();
DWORD GetTickCount();
DWORD timeGetTime();
UINT timeBeginPeriod(UINT ms);
UINT timeEndPeriod(UINT ms);
#define TIMERR_NOERROR 0
void GetSystemTimeAsFileTime(LPFILETIME ft);
void GetSystemTimePreciseAsFileTime(LPFILETIME ft);
void GetSystemTime(LPSYSTEMTIME st);
void GetLocalTime(LPSYSTEMTIME st);
BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st);
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft);
BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local);
BOOL QueryUnbiasedInterruptTime(PULONGLONG t);

// ---- processor information ----
typedef enum _LOGICAL_PROCESSOR_RELATIONSHIP {
    RelationProcessorCore = 0, RelationNumaNode = 1, RelationCache = 2, RelationProcessorPackage = 3, RelationGroup = 4,
    RelationProcessorDie = 5, RelationNumaNodeEx = 6, RelationProcessorModule = 7, RelationAll = 0xffff
} LOGICAL_PROCESSOR_RELATIONSHIP;
typedef ULONG_PTR KAFFINITY;
typedef struct _GROUP_AFFINITY { KAFFINITY Mask; WORD Group; WORD Reserved[3]; } GROUP_AFFINITY, *PGROUP_AFFINITY;
typedef struct _PROCESSOR_RELATIONSHIP {
    BYTE Flags; BYTE EfficiencyClass; BYTE Reserved[20]; WORD GroupCount; GROUP_AFFINITY GroupMask[1];
} PROCESSOR_RELATIONSHIP, *PPROCESSOR_RELATIONSHIP;
typedef enum _PROCESSOR_CACHE_TYPE { CacheUnified, CacheInstruction, CacheData, CacheTrace } PROCESSOR_CACHE_TYPE;
typedef struct _CACHE_RELATIONSHIP {
    BYTE Level; BYTE Associativity; WORD LineSize; DWORD CacheSize; PROCESSOR_CACHE_TYPE Type; BYTE Reserved[18]; WORD GroupCount;
    GROUP_AFFINITY GroupMask;
} CACHE_RELATIONSHIP, *PCACHE_RELATIONSHIP;
typedef struct _NUMA_NODE_RELATIONSHIP { DWORD NodeNumber; BYTE Reserved[18]; WORD GroupCount; GROUP_AFFINITY GroupMask; } NUMA_NODE_RELATIONSHIP;
typedef struct _PROCESSOR_GROUP_INFO { BYTE MaximumProcessorCount; BYTE ActiveProcessorCount; BYTE Reserved[38]; KAFFINITY ActiveProcessorMask; } PROCESSOR_GROUP_INFO;
typedef struct _GROUP_RELATIONSHIP { WORD MaximumGroupCount; WORD ActiveGroupCount; BYTE Reserved[20]; PROCESSOR_GROUP_INFO GroupInfo[1]; } GROUP_RELATIONSHIP;
typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    DWORD Size;
    union {
        PROCESSOR_RELATIONSHIP Processor;
        NUMA_NODE_RELATIONSHIP NumaNode;
        CACHE_RELATIONSHIP Cache;
        GROUP_RELATIONSHIP Group;
    };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;
// Reports one core per performance level-0 (P) core with EfficiencyClass 1 and one per E core with EfficiencyClass 0, each with one
// logical processor; logical processor numbers are P cores first.
BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf, PDWORD len);
typedef struct _PROCESSOR_NUMBER { WORD Group; BYTE Number; BYTE Reserved; } PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;
DWORD GetCurrentProcessorNumber();
void GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER n);
BOOL SetThreadIdealProcessorEx(HANDLE h, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER prev);
BOOL GetThreadIdealProcessorEx(HANDLE h, PPROCESSOR_NUMBER ideal);
DWORD SetThreadIdealProcessor(HANDLE h, DWORD ideal);
BOOL SetThreadSelectedCpuSets(HANDLE h, const ULONG* ids, ULONG n);
DWORD GetActiveProcessorCount(WORD group);
#define ALL_PROCESSOR_GROUPS 0xffff
typedef enum _THREAD_INFORMATION_CLASS { ThreadMemoryPriority, ThreadAbsoluteCpuPriority, ThreadDynamicCodePolicy, ThreadPowerThrottling } THREAD_INFORMATION_CLASS;
typedef enum _PROCESS_INFORMATION_CLASS { ProcessMemoryPriority, ProcessMemoryExhaustionInfo, ProcessAppMemoryInfo, ProcessInPrivateInfo,
    ProcessPowerThrottling } PROCESS_INFORMATION_CLASS;
typedef struct _THREAD_POWER_THROTTLING_STATE { ULONG Version, ControlMask, StateMask; } THREAD_POWER_THROTTLING_STATE;
typedef struct _PROCESS_POWER_THROTTLING_STATE { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1
#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
BOOL SetThreadInformation(HANDLE h, THREAD_INFORMATION_CLASS cls, LPVOID info, DWORD size);
BOOL SetProcessInformation(HANDLE h, PROCESS_INFORMATION_CLASS cls, LPVOID info, DWORD size);

// ---- user interface and shell ----
#define MB_OK 0x0
#define MB_OKCANCEL 0x1
#define MB_YESNO 0x4
#define MB_ICONERROR 0x10
#define MB_ICONHAND 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONEXCLAMATION 0x30
#define MB_ICONINFORMATION 0x40
#define MB_SETFOREGROUND 0x10000
#define MB_TOPMOST 0x40000
#define MB_SYSTEMMODAL 0x1000
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type);
int MessageBoxW(HWND w, LPCWSTR text, LPCWSTR caption, UINT type);
HINSTANCE ShellExecuteA(HWND w, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show);
HINSTANCE ShellExecuteW(HWND w, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show);

// ---- locale ----
typedef WORD LANGID;
typedef DWORD LCID;
#define LOCALE_NAME_MAX_LENGTH 85
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define PRIMARYLANGID(l) ((WORD)(l) & 0x3ff)
#define SUBLANGID(l) ((WORD)(l) >> 10)
// The user's preferred UI language as a Windows LANGID (from CFLocale's preferred languages).
LANGID GetUserDefaultUILanguage();
LANGID GetUserDefaultLangID();
LANGID GetSystemDefaultUILanguage();
int GetUserDefaultLocaleName(LPWSTR buf, int n);
