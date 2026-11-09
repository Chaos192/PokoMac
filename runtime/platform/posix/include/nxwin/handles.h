// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: kernel objects behind HANDLE (threads, events, mutexes, waitable timers, processes, files, sections) and the waits on them.
// A HANDLE is a pointer to a reference-counted nxwin::Object; CloseHandle drops one reference. GetCurrentThread / GetCurrentProcess
// return the Win32 pseudo handles (-2 / -1), which DuplicateHandle turns into real ones.
#pragma once
#include "nxwin/types.h"

#define WAIT_OBJECT_0 0x00000000u
#define WAIT_ABANDONED 0x00000080u
#define WAIT_ABANDONED_0 0x00000080u
#define WAIT_TIMEOUT 0x00000102u
#define WAIT_FAILED 0xFFFFFFFFu
#define WAIT_IO_COMPLETION 0x000000C0u
#define MAXIMUM_WAIT_OBJECTS 64

#define STILL_ACTIVE 259
#define CREATE_SUSPENDED 0x00000004
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_ALL_ACCESS 0x001FFFFF
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_SET_CONTEXT 0x0010
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_SET_INFORMATION 0x0020
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#define THREAD_SET_LIMITED_INFORMATION 0x0400
#define PROCESS_ALL_ACCESS 0x001FFFFF
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_TERMINATE 0x0001
#define PROCESS_VM_READ 0x0010
#define SYNCHRONIZE 0x00100000
#define DUPLICATE_CLOSE_SOURCE 0x1
#define DUPLICATE_SAME_ACCESS 0x2
#define EVENT_ALL_ACCESS 0x1F0003
#define EVENT_MODIFY_STATE 0x0002
#define MUTEX_ALL_ACCESS 0x1F0001
#define TIMER_ALL_ACCESS 0x1F0003
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define CREATE_EVENT_MANUAL_RESET 0x00000001
#define CREATE_EVENT_INITIAL_SET 0x00000002

#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define THREAD_MODE_BACKGROUND_END 0x00020000
#define IDLE_PRIORITY_CLASS 0x00000040
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define NORMAL_PRIORITY_CLASS 0x00000020
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#define HIGH_PRIORITY_CLASS 0x00000080
#define REALTIME_PRIORITY_CLASS 0x00000100
#define PROCESS_MODE_BACKGROUND_BEGIN 0x00100000
#define PROCESS_MODE_BACKGROUND_END 0x00200000
#define CREATE_NO_WINDOW 0x08000000
#define DETACHED_PROCESS 0x00000008
#define CREATE_NEW_PROCESS_GROUP 0x00000200

typedef DWORD(WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);
typedef void(CALLBACK* PTIMERAPCROUTINE)(LPVOID, DWORD, DWORD);
typedef struct _REASON_CONTEXT { ULONG Version; DWORD Flags; void* Reason; } REASON_CONTEXT, *PREASON_CONTEXT;

namespace nxwin {
struct Object;
Object* object_of(HANDLE h);        // nullptr for pseudo handles and invalid values
HANDLE handle_of(Object* o);        // adds no reference
uint64_t thread_key();              // a unique id of the calling thread (never 0)
// mach port of the thread behind a thread handle (pseudo handle: the caller), 0 if none / exited.
unsigned thread_port(HANDLE h);
// Runs fn(arg) on a new thread that has an alternate signal stack and the nxwin thread state; returns false on failure.
bool spawn_detached(void (*fn)(void*), void* arg, size_t stack_bytes, const char* name);
}  // namespace nxwin

BOOL CloseHandle(HANDLE h);
BOOL DuplicateHandle(HANDLE src_proc, HANDLE src, HANDLE dst_proc, LPHANDLE out, DWORD access, BOOL inherit, DWORD options);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms);
DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alertable);

// threads
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid);
[[noreturn]] void ExitThread(DWORD code);
DWORD ResumeThread(HANDLE h);
DWORD SuspendThread(HANDLE h);
BOOL TerminateThread(HANDLE h, DWORD code);
BOOL GetExitCodeThread(HANDLE h, LPDWORD code);
HANDLE GetCurrentThread();
DWORD GetCurrentThreadId();
DWORD GetThreadId(HANDLE h);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
BOOL SetThreadPriorityBoost(HANDLE h, BOOL disable);
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
BOOL SetThreadStackGuarantee(PULONG size);
HRESULT SetThreadDescription(HANDLE h, PCWSTR name);
BOOL SwitchToThread();
void Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);
BOOL GetThreadTimes(HANDLE h, LPFILETIME create, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
BOOL QueryThreadCycleTime(HANDLE h, PULONG64 cycles);

// events and mutexes
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name);
HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
#define CreateEvent CreateEventW
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCSTR name);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCWSTR name);
#define CreateMutex CreateMutexW
BOOL ReleaseMutex(HANDLE h);

// waitable timers (due < 0: relative, in 100 ns units; >= 0: absolute FILETIME)
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE apc, LPVOID arg, BOOL resume);
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE apc, LPVOID arg, PREASON_CONTEXT ctx, ULONG delay);
BOOL CancelWaitableTimer(HANDLE h);

// processes
typedef struct _STARTUPINFOA { DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars,
    dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct _STARTUPINFOW { DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars,
    dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOW, *LPSTARTUPINFOW;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
#define STARTF_USESHOWWINDOW 0x00000001
#define STARTF_USESTDHANDLES 0x00000100
#define SW_HIDE 0
#define SW_SHOW 5
#define SW_SHOWNORMAL 1
BOOL CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir,
                    LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi);
BOOL CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir,
                    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi);
HANDLE GetCurrentProcess();
DWORD GetCurrentProcessId();
DWORD GetProcessId(HANDLE h);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
BOOL TerminateProcess(HANDLE h, UINT code);
[[noreturn]] void ExitProcess(UINT code);
BOOL GetExitCodeProcess(HANDLE h, LPDWORD code);
BOOL SetPriorityClass(HANDLE h, DWORD cls);
DWORD GetPriorityClass(HANDLE h);
BOOL GetProcessTimes(HANDLE h, LPFILETIME create, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
BOOL GetProcessAffinityMask(HANDLE h, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask);
BOOL GetProcessHandleCount(HANDLE h, PDWORD count);
DWORD GetGuiResources(HANDLE h, DWORD flags);
