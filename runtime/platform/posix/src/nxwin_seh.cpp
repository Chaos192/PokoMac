// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: structured exceptions over POSIX signals (see nxwin/seh.h for the dispatch order).
// The signal handler runs on the thread's alternate stack, reads only lock-free state (the handler snapshot, the thread's __try chain
// in a pthread key) and allocates nothing, so it may interrupt any code.
#include "nxwin_internal.h"

#include <execinfo.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <atomic>

namespace nxwin {
namespace {

constexpr int kMaxHandlers = 16;
struct HandlerList {
    int n = 0;
    PVECTORED_EXCEPTION_HANDLER h[kMaxHandlers] = {};
};
std::atomic<const HandlerList*> g_handlers{nullptr};
std::mutex g_handlers_m;
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_filter{nullptr};
pthread_key_t g_seh_key;
pthread_once_t g_once = PTHREAD_ONCE_INIT;
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
struct sigaction g_prev[32];

SehFrame* seh_top() { return static_cast<SehFrame*>(pthread_getspecific(g_seh_key)); }
void set_seh_top(SehFrame* f) { pthread_setspecific(g_seh_key, f); }

#if defined(__aarch64__)
void to_context(const ucontext_t* uc, CONTEXT* c) {
    const auto& ss = uc->uc_mcontext->__ss;
    c->ContextFlags = CONTEXT_FULL;
    for (int i = 0; i < 29; ++i) c->X[i] = ss.__x[i];
    c->Fp = arm_thread_state64_get_fp(ss);
    c->Lr = arm_thread_state64_get_lr(ss);
    c->Sp = arm_thread_state64_get_sp(ss);
    c->Pc = arm_thread_state64_get_pc(ss);
    c->Cpsr = ss.__cpsr;
    const auto& ns = uc->uc_mcontext->__ns;
    for (int i = 0; i < 32; ++i) std::memcpy(&c->V[i], &ns.__v[i], 16);
    c->Fpcr = ns.__fpcr;
    c->Fpsr = ns.__fpsr;
}
void from_context(const CONTEXT* c, ucontext_t* uc) {
    auto& ss = uc->uc_mcontext->__ss;
    for (int i = 0; i < 29; ++i) ss.__x[i] = c->X[i];
    arm_thread_state64_set_fp(ss, c->Fp);
    arm_thread_state64_set_lr_fptr(ss, reinterpret_cast<void*>(c->Lr));
    arm_thread_state64_set_sp(ss, c->Sp);
    arm_thread_state64_set_pc_fptr(ss, reinterpret_cast<void*>(c->Pc));
    ss.__cpsr = c->Cpsr;
}
void thread_state_to_context(const arm_thread_state64_t& ss, CONTEXT* c) {
    c->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    for (int i = 0; i < 29; ++i) c->X[i] = ss.__x[i];
    c->Fp = arm_thread_state64_get_fp(ss);
    c->Lr = arm_thread_state64_get_lr(ss);
    c->Sp = arm_thread_state64_get_sp(ss);
    c->Pc = arm_thread_state64_get_pc(ss);
    c->Cpsr = ss.__cpsr;
}
#endif

// Whether the fault address lies in the guard area below the thread's stack.
bool is_stack_overflow(uint64_t far) {
    pthread_t self = pthread_self();
    const uint64_t top = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
    const uint64_t size = pthread_get_stacksize_np(self);
    const uint64_t bottom = top - size;
    return far < bottom && far + (1u << 20) >= bottom;
}

void restore_default_and_return(int sig) {
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

LONG dispatch(EXCEPTION_RECORD* rec, CONTEXT* ctx, bool allow_frames) {
    EXCEPTION_POINTERS ep{rec, ctx};
    if (const HandlerList* l = g_handlers.load(std::memory_order_acquire)) {
        for (int i = 0; i < l->n; ++i) {
            if (!l->h[i]) continue;
            if (l->h[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (allow_frames) {
        if (SehFrame* f = seh_top()) {
            set_seh_top(f->prev);
            f->active = false;
            f->code = rec->ExceptionCode;
            f->addr = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
            siglongjmp(f->jb, 1);
        }
    }
    if (LPTOP_LEVEL_EXCEPTION_FILTER flt = g_filter.load(std::memory_order_acquire)) return flt(&ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_signal(int sig, siginfo_t* si, void* uc_v) {
    const int saved_errno = errno;
    auto* uc = static_cast<ucontext_t*>(uc_v);
    EXCEPTION_RECORD rec{};
    CONTEXT ctx{};
#if defined(__aarch64__)
    to_context(uc, &ctx);
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Pc);
    const uint32_t esr = uc->uc_mcontext->__es.__esr;
    const uint64_t far = uc->uc_mcontext->__es.__far;
    const uint32_t ec = esr >> 26;
#else
    const uint32_t esr = 0, ec = 0;
    const uint64_t far = reinterpret_cast<uint64_t>(si->si_addr);
#endif
    switch (sig) {
        case SIGSEGV:
        case SIGBUS:
            rec.ExceptionCode = is_stack_overflow(far) ? EXCEPTION_STACK_OVERFLOW : EXCEPTION_ACCESS_VIOLATION;
            rec.NumberParameters = 2;
            rec.ExceptionInformation[0] = (ec == 0x20 || ec == 0x21) ? 8 : ((ec == 0x24 || ec == 0x25) && (esr >> 6 & 1)) ? 1 : 0;
            rec.ExceptionInformation[1] = far ? far : reinterpret_cast<ULONG_PTR>(si->si_addr);
            break;
        case SIGILL: rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION; break;
        case SIGFPE: rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO; break;
        case SIGTRAP:
#if defined(__aarch64__)
            if (ec == 0x34 || ec == 0x35) {      // a watchpoint (lower / same EL): a data breakpoint, as Windows reports one
                rec.ExceptionCode = EXCEPTION_SINGLE_STEP;
                rec.NumberParameters = 2;
                rec.ExceptionInformation[0] = (esr >> 6 & 1) ? 1 : 0;   // WnR: 1 a store
                rec.ExceptionInformation[1] = far;
                break;
            }
#endif
            rec.ExceptionCode = EXCEPTION_BREAKPOINT;
            break;
        case SIGABRT: rec.ExceptionCode = STATUS_FATAL_APP_EXIT; rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE; break;
        default: rec.ExceptionCode = 0xC0000000u | static_cast<DWORD>(sig); break;
    }
    const LONG r = dispatch(&rec, &ctx, sig != SIGABRT && sig != SIGTRAP);
    if (r == EXCEPTION_CONTINUE_EXECUTION && sig != SIGABRT) {
#if defined(__aarch64__)
        from_context(&ctx, uc);
#endif
        errno = saved_errno;
        return;
    }
    // Unhandled: let the system's default action end the process (a crash report from macOS, as with WER on Windows).
    restore_default_and_return(sig);
    if (sig == SIGABRT || sig == SIGTRAP) raise(sig);
    errno = saved_errno;
}

void install_handlers() {
    pthread_key_create(&g_seh_key, nullptr);
    signal(SIGPIPE, SIG_IGN);
    for (int sig : kSignals) {
        struct sigaction sa{};
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &g_prev[sig]);
    }
}

}  // namespace

SehFrame::SehFrame() {
    pthread_once(&g_once, install_handlers);
    prev = seh_top();
    active = true;
    set_seh_top(this);
}
SehFrame::~SehFrame() {
    if (active) {
        set_seh_top(prev);
        active = false;
    }
}

void seh_init_thread() {
    pthread_once(&g_once, install_handlers);
    stack_t cur{};
    if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_sp) return;
    const size_t sz = 256u << 10;
    void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss{};
    ss.ss_sp = p;
    ss.ss_size = sz;
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
}

// The main thread gets its handlers and alternate stack when the runtime image loads.
__attribute__((constructor)) static void nxwin_seh_boot() {
    seh_init_thread();
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

}  // namespace nxwin

using namespace nxwin;

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    pthread_once(&g_once, install_handlers);
    std::lock_guard<std::mutex> g(g_handlers_m);
    const HandlerList* old = g_handlers.load();
    auto* l = new HandlerList(old ? *old : HandlerList{});      // the old list is never freed: a handler may be reading it
    if (l->n >= kMaxHandlers) return nullptr;
    if (first) {
        for (int i = l->n; i > 0; --i) l->h[i] = l->h[i - 1];
        l->h[0] = h;
    } else {
        l->h[l->n] = h;
    }
    ++l->n;
    g_handlers.store(l, std::memory_order_release);
    return reinterpret_cast<PVOID>(h);
}
ULONG RemoveVectoredExceptionHandler(PVOID cookie) {
    std::lock_guard<std::mutex> g(g_handlers_m);
    const HandlerList* old = g_handlers.load();
    if (!old) return 0;
    auto* l = new HandlerList;
    bool found = false;
    for (int i = 0; i < old->n; ++i) {
        if (!found && reinterpret_cast<PVOID>(old->h[i]) == cookie) { found = true; continue; }
        l->h[l->n++] = old->h[i];
    }
    g_handlers.store(l, std::memory_order_release);
    return found ? 1 : 0;
}
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    pthread_once(&g_once, install_handlers);
    return g_filter.exchange(f);
}
LONG UnhandledExceptionFilter(PEXCEPTION_POINTERS) { return EXCEPTION_EXECUTE_HANDLER; }

void RtlCaptureContext(PCONTEXT ctx) {
    std::memset(ctx, 0, sizeof *ctx);
    ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
#if defined(__aarch64__)
    uint64_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    ctx->Sp = sp;
    ctx->Fp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    ctx->Lr = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    ctx->Pc = ctx->Lr;
#endif
}

void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args) {
    EXCEPTION_RECORD rec{};
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags;
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Pc);
    rec.NumberParameters = std::min<DWORD>(nargs, EXCEPTION_MAXIMUM_PARAMETERS);
    for (DWORD i = 0; i < rec.NumberParameters; ++i) rec.ExceptionInformation[i] = args[i];
    const LONG r = dispatch(&rec, &ctx, true);
    if (r == EXCEPTION_CONTINUE_EXECUTION && !(flags & EXCEPTION_NONCONTINUABLE)) return;
    _exit(3);
}

BOOL GetThreadContext(HANDLE thread, LPCONTEXT ctx) {
    const unsigned port = thread_port(thread);
    if (!port) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (port == pthread_mach_thread_np(pthread_self())) { RtlCaptureContext(ctx); return TRUE; }
#if defined(__aarch64__)
    arm_thread_state64_t ss;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&ss), &n) != KERN_SUCCESS) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const DWORD want = ctx->ContextFlags;
    thread_state_to_context(ss, ctx);
    ctx->ContextFlags = want;
    return TRUE;
#else
    return FALSE;
#endif
}
BOOL SetThreadContext(HANDLE thread, const CONTEXT* ctx) {
    const unsigned port = thread_port(thread);
    if (!port) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
#if defined(__aarch64__)
    arm_thread_state64_t ss;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&ss), &n) != KERN_SUCCESS) return FALSE;
    for (int i = 0; i < 29; ++i) ss.__x[i] = ctx->X[i];
    arm_thread_state64_set_fp(ss, ctx->Fp);
    arm_thread_state64_set_lr_fptr(ss, reinterpret_cast<void*>(ctx->Lr));
    arm_thread_state64_set_sp(ss, ctx->Sp);
    arm_thread_state64_set_pc_fptr(ss, reinterpret_cast<void*>(ctx->Pc));
    return thread_set_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&ss), n) == KERN_SUCCESS;
#else
    return FALSE;
#endif
}
USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG n, PVOID* frames, PULONG hash) {
    void* buf[128];
    int got = backtrace(buf, 128);
    int start = static_cast<int>(skip) + 1;
    int k = 0;
    for (int i = start; i < got && k < static_cast<int>(n); ++i) frames[k++] = buf[i];
    if (hash) {
        ULONG h = 0;
        for (int i = 0; i < k; ++i) h += static_cast<ULONG>(reinterpret_cast<uintptr_t>(frames[i]));
        *hash = h;
    }
    return static_cast<USHORT>(k);
}
BOOL IsDebuggerPresent() {
    struct kinfo_proc info{};
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}
void DebugBreak() { __builtin_debugtrap(); }
void OutputDebugStringA(LPCSTR s) {
    if (IsDebuggerPresent() && s) fputs(s, stderr);
}
void OutputDebugStringW(LPCWSTR s) {
    if (IsDebuggerPresent() && s) fputs(narrow(s).c_str(), stderr);
}
