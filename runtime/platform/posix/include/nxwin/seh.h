// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin: structured exceptions over POSIX signals.
//  - SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGTRAP become an EXCEPTION_RECORD + CONTEXT and go, in Windows order, to the vectored
//    handlers (AddVectoredExceptionHandler), then to the innermost __try frame of the faulting thread, then to the unhandled
//    exception filter; a handler that returns EXCEPTION_CONTINUE_EXECUTION resumes at the (possibly changed) context.
//  - __try / __except is a sigsetjmp frame: the filter expression is not evaluated, every fault in the block runs the handler.
//    Objects with destructors inside a __try block are skipped by the jump, as they would be with a hardware fault under MSVC.
//  - CONTEXT is the Windows-on-ARM64 layout (Pc, Sp, Fp, Lr, X[]), so code written for x64 registers does not compile by accident.
#pragma once
#include <csetjmp>

#include "nxwin/types.h"

#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_DATATYPE_MISALIGNMENT 0x80000002u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_IN_PAGE_ERROR 0xC0000006u
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_FLT_DIVIDE_BY_ZERO 0xC000008Eu
#define EXCEPTION_FLT_INVALID_OPERATION 0xC0000090u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_PRIV_INSTRUCTION 0xC0000096u
#define EXCEPTION_GUARD_PAGE 0x80000001u
#define STATUS_ACCESS_VIOLATION EXCEPTION_ACCESS_VIOLATION
#define STATUS_STACK_OVERFLOW EXCEPTION_STACK_OVERFLOW
#define STATUS_HEAP_CORRUPTION 0xC0000374u
#define STATUS_STACK_BUFFER_OVERRUN 0xC0000409u
#define STATUS_FATAL_APP_EXIT 0x40000015u
#define DBG_PRINTEXCEPTION_C 0x40010006u
#define DBG_PRINTEXCEPTION_WIDE_C 0x4001000Au
#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

#define CONTEXT_ARM64 0x00400000L
#define CONTEXT_CONTROL (CONTEXT_ARM64 | 0x1L)
#define CONTEXT_INTEGER (CONTEXT_ARM64 | 0x2L)
#define CONTEXT_FLOATING_POINT (CONTEXT_ARM64 | 0x4L)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_ARM64 | 0x8L)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL CONTEXT_FULL

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct alignas(16) _CONTEXT {
    DWORD ContextFlags;
    DWORD Cpsr;
    union {
        struct {
            DWORD64 X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X16, X17, X18, X19, X20, X21, X22, X23, X24,
                X25, X26, X27, X28, Fp, Lr;
        };
        DWORD64 X[31];
    };
    DWORD64 Sp;
    DWORD64 Pc;
    struct { uint64_t Low; int64_t High; } V[32];
    DWORD Fpcr;
    DWORD Fpsr;
} CONTEXT, *PCONTEXT, *LPCONTEXT;

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;

typedef LONG(CALLBACK* PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS);
typedef LONG(WINAPI* PTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
ULONG RemoveVectoredExceptionHandler(PVOID cookie);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
LONG UnhandledExceptionFilter(PEXCEPTION_POINTERS ep);
void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args);
void RtlCaptureContext(PCONTEXT ctx);
BOOL GetThreadContext(HANDLE thread, LPCONTEXT ctx);
BOOL SetThreadContext(HANDLE thread, const CONTEXT* ctx);
USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG n, PVOID* frames, PULONG hash);
#define CaptureStackBackTrace RtlCaptureStackBackTrace
BOOL IsDebuggerPresent();
void DebugBreak();
#define __debugbreak() __builtin_debugtrap()
void OutputDebugStringA(LPCSTR s);
void OutputDebugStringW(LPCWSTR s);

namespace nxwin {
struct SehFrame {
    sigjmp_buf jb;
    SehFrame* prev;
    DWORD code = 0;
    ULONG_PTR addr = 0;
    bool active;
    SehFrame();
    ~SehFrame();
};
// Installs the signal handlers and this thread's alternate signal stack (idempotent per thread). Threads made through
// CreateThread / std::thread on nxwin call it themselves; call it once on the main thread at startup.
void seh_init_thread();
}  // namespace nxwin

#define __try if (::nxwin::SehFrame _nxwin_seh; sigsetjmp(_nxwin_seh.jb, 0) == 0)
#define __except(...) else
#define __finally
#define __leave
#define GetExceptionCode() (_nxwin_seh.code)
#define GetExceptionInformation() (static_cast<PEXCEPTION_POINTERS>(nullptr))
