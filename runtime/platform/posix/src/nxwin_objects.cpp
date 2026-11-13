// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Chaos192
// nxwin: kernel objects (threads, events, mutexes, waitable timers, processes, thread snapshots) and waits.
#include "nxwin_internal.h"

#include <avrt.h>
#include <tlhelp32.h>

#include <dirent.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>

extern char** environ;

namespace nxwin {

Object::~Object() { magic = 0; }

void Object::signal_locked() {
    for (Waiter* w : waiters) {
        std::lock_guard<std::mutex> g(w->m);
        w->woke = true;
        w->cv.notify_one();
    }
}

namespace {
constexpr intptr_t kPseudoProcess = -1, kPseudoThread = -2;
}

Object* resolve(HANDLE h, int* pseudo) {
    const auto v = reinterpret_cast<intptr_t>(h);
    if (pseudo) *pseudo = 0;
    if (v == kPseudoProcess || v == kPseudoThread) {
        if (pseudo) *pseudo = static_cast<int>(v);
        return nullptr;
    }
    if (!h || (v & 7)) return nullptr;
    auto* o = static_cast<Object*>(h);
    return o->magic == Object::kMagic ? o : nullptr;
}
Object* object_of(HANDLE h) { return resolve(h); }
HANDLE handle_of(Object* o) { return static_cast<HANDLE>(o); }

uint64_t thread_key() {
    static thread_local uint64_t k = [] {
        uint64_t id = 0;
        pthread_threadid_np(nullptr, &id);
        return id ? id : 1;
    }();
    return k;
}

void set_errno_error() { SetLastError(errno_to_win32(errno)); }

// ---- threads -------------------------------------------------------------------------------------------------------------------------------
struct ThreadObj final : Object {
    pthread_t th{};
    mach_port_t port = MACH_PORT_NULL;
    LPTHREAD_START_ROUTINE fn = nullptr;
    void* arg = nullptr;
    bool done = false;
    bool gated = false;          // CREATE_SUSPENDED and not resumed yet
    int gate_count = 0;
    DWORD exit_code = STILL_ACTIVE;
    std::condition_variable gate_cv;
    uint64_t create_ns = 0;
    bool waitable() const override { return true; }
    bool try_acquire(uint64_t) override { return done; }
};

namespace {
thread_local ThreadObj* t_self = nullptr;
std::mutex g_threads_m;
std::map<DWORD, ThreadObj*> g_threads;   // live threads by id (weak: the object removes itself when it finishes)

pthread_key_t g_exit_key;
void finish(ThreadObj* t, DWORD code) {
    {
        std::lock_guard<std::mutex> g(g_threads_m);
        auto it = g_threads.find(static_cast<DWORD>(t->port));
        if (it != g_threads.end() && it->second == t) g_threads.erase(it);
    }
    {
        std::lock_guard<std::mutex> g(t->m);
        if (t->done) return;
        t->exit_code = code;
        t->done = true;
        t->signal_locked();
    }
}
void on_thread_exit(void* p) {           // TLS destructor: threads that end without ExitThread (return from the entry, std::thread)
    auto* t = static_cast<ThreadObj*>(p);
    finish(t, t->exit_code == STILL_ACTIVE ? 0 : t->exit_code);
    t->unref();                         // the reference the thread held on itself
}
pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
void make_key() { pthread_key_create(&g_exit_key, on_thread_exit); }

// The caller hands over one reference to `t`: the thread's own, released by on_thread_exit.
void register_self(ThreadObj* t) {
    pthread_once(&g_key_once, make_key);
    t_self = t;
    pthread_setspecific(g_exit_key, t);
    std::lock_guard<std::mutex> g(g_threads_m);
    g_threads[static_cast<DWORD>(t->port)] = t;
}

void* thread_entry(void* p) {
    auto* t = static_cast<ThreadObj*>(p);
    register_self(t);
    seh_init_thread();
    {
        std::unique_lock<std::mutex> lk(t->m);
        t->gate_cv.wait(lk, [t] { return !t->gated; });
    }
    const DWORD code = t->fn(t->arg);
    t->exit_code = code;
    return nullptr;                     // on_thread_exit finishes the object
}
}  // namespace

ThreadObj* current_thread_obj() {
    if (t_self) return t_self;
    auto* t = new ThreadObj;
    t->th = pthread_self();
    t->port = pthread_mach_thread_np(t->th);
    t->create_ns = mono_ns();
    register_self(t);                   // the creation reference becomes the thread's own
    return t;
}
unsigned thread_port_of(ThreadObj* t) { return t ? t->port : 0; }
unsigned thread_port(HANDLE h) {
    int pseudo = 0;
    Object* o = resolve(h, &pseudo);
    if (pseudo == kPseudoThread) return pthread_mach_thread_np(pthread_self());
    auto* t = dynamic_cast<ThreadObj*>(o);
    if (!t) return 0;
    std::lock_guard<std::mutex> g(t->m);
    return t->done ? 0 : t->port;
}

bool spawn_detached(void (*fn)(void*), void* arg, size_t stack_bytes, const char*) {
    struct Box { void (*fn)(void*); void* arg; };
    auto* b = new Box{fn, arg};
    HANDLE h = CreateThread(nullptr, stack_bytes, [](LPVOID p) -> DWORD {
        std::unique_ptr<Box> box(static_cast<Box*>(p));
        box->fn(box->arg);
        return 0;
    }, b, 0, nullptr);
    if (!h) { delete b; return false; }
    CloseHandle(h);
    return true;
}

}  // namespace nxwin

using namespace nxwin;

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid) {
    auto* t = new ThreadObj;
    t->fn = fn;
    t->arg = arg;
    t->gated = flags & CREATE_SUSPENDED;
    t->gate_count = t->gated ? 1 : 0;
    t->create_ns = mono_ns();
    pthread_attr_t a;
    pthread_attr_init(&a);
    size_t ss = stack ? stack : (8u << 20);
    const size_t pg = static_cast<size_t>(getpagesize());
    ss = (std::max<size_t>(ss, 256u << 10) + pg - 1) & ~(pg - 1);
    pthread_attr_setstacksize(&a, ss);
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    t->ref();                                          // the thread's own reference (handed to register_self)
    const int r = pthread_create(&t->th, &a, thread_entry, t);
    pthread_attr_destroy(&a);
    if (r != 0) {
        SetLastError(errno_to_win32(r));
        t->unref();
        t->unref();
        return nullptr;
    }
    t->port = pthread_mach_thread_np(t->th);
    pthread_detach(t->th);
    {
        std::lock_guard<std::mutex> g(g_threads_m);
        g_threads[static_cast<DWORD>(t->port)] = t;
    }
    if (tid) *tid = static_cast<DWORD>(t->port);
    return handle_of(t);
}

void ExitThread(DWORD code) {
    if (t_self) t_self->exit_code = code;
    pthread_exit(nullptr);
}

DWORD ResumeThread(HANDLE h) {
    auto* t = as<ThreadObj>(h);
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return static_cast<DWORD>(-1); }
    {
        std::lock_guard<std::mutex> g(t->m);
        if (t->gated) {
            const DWORD prev = static_cast<DWORD>(t->gate_count);
            if (--t->gate_count <= 0) {
                t->gated = false;
                t->gate_cv.notify_all();
            }
            return prev;
        }
        if (t->done) return 0;
    }
    struct thread_basic_info info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    const DWORD prev = thread_info(t->port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) == KERN_SUCCESS
                           ? static_cast<DWORD>(info.suspend_count) : 0;
    if (prev) thread_resume(t->port);
    return prev;
}

DWORD SuspendThread(HANDLE h) {
    int pseudo = 0;
    Object* o = resolve(h, &pseudo);
    auto* t = dynamic_cast<ThreadObj*>(o);
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return static_cast<DWORD>(-1); }
    {
        std::lock_guard<std::mutex> g(t->m);
        if (t->gated) return static_cast<DWORD>(t->gate_count++);
        if (t->done) { SetLastError(ERROR_ACCESS_DENIED); return static_cast<DWORD>(-1); }
    }
    struct thread_basic_info info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    const DWORD prev = thread_info(t->port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) == KERN_SUCCESS
                           ? static_cast<DWORD>(info.suspend_count) : 0;
    if (thread_suspend(t->port) != KERN_SUCCESS) { SetLastError(ERROR_ACCESS_DENIED); return static_cast<DWORD>(-1); }
    return prev;
}

BOOL TerminateThread(HANDLE, DWORD) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
BOOL GetExitCodeThread(HANDLE h, LPDWORD code) {
    auto* t = as<ThreadObj>(h);
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::lock_guard<std::mutex> g(t->m);
    *code = t->done ? t->exit_code : STILL_ACTIVE;
    return TRUE;
}
HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(kPseudoThread); }
DWORD GetCurrentThreadId() { return static_cast<DWORD>(pthread_mach_thread_np(pthread_self())); }
DWORD GetThreadId(HANDLE h) {
    int pseudo = 0;
    Object* o = resolve(h, &pseudo);
    if (pseudo == kPseudoThread) return GetCurrentThreadId();
    auto* t = dynamic_cast<ThreadObj*>(o);
    return t ? static_cast<DWORD>(t->port) : 0;
}
HANDLE OpenThread(DWORD, BOOL, DWORD tid) {
    if (tid == GetCurrentThreadId()) {
        ThreadObj* t = current_thread_obj();
        t->ref();
        return handle_of(t);
    }
    std::lock_guard<std::mutex> g(g_threads_m);
    auto it = g_threads.find(tid);
    if (it == g_threads.end()) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    it->second->ref();
    return handle_of(it->second);
}

// Priorities: only the calling thread's QoS class can be changed on macOS; requests for other threads succeed without effect.
namespace {
thread_local int t_prio = THREAD_PRIORITY_NORMAL;
bool is_self(HANDLE h) {
    int pseudo = 0;
    Object* o = resolve(h, &pseudo);
    return pseudo == kPseudoThread || (o && o == t_self);
}
}  // namespace
BOOL SetThreadPriority(HANDLE h, int prio) {
    if (!is_self(h)) return TRUE;
    if (prio == THREAD_MODE_BACKGROUND_BEGIN) { pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0); return TRUE; }
    if (prio == THREAD_MODE_BACKGROUND_END) prio = THREAD_PRIORITY_NORMAL;
    t_prio = prio;
    const qos_class_t q = prio <= THREAD_PRIORITY_IDLE ? QOS_CLASS_UTILITY
                          : prio < THREAD_PRIORITY_NORMAL ? QOS_CLASS_USER_INITIATED : QOS_CLASS_USER_INTERACTIVE;
    pthread_set_qos_class_self_np(q, 0);
    return TRUE;
}
int GetThreadPriority(HANDLE h) { return is_self(h) ? t_prio : THREAD_PRIORITY_NORMAL; }
BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR) { return ~DWORD_PTR{0}; }
BOOL SetThreadStackGuarantee(PULONG size) { if (size) *size = 0; return TRUE; }
HRESULT SetThreadDescription(HANDLE h, PCWSTR name) {
    if (is_self(h)) pthread_setname_np(narrow(name).c_str());
    return S_OK;
}
BOOL SwitchToThread() { sched_yield(); return TRUE; }
void Sleep(DWORD ms) {
    if (ms == 0) { sched_yield(); return; }
    if (ms == INFINITE) { for (;;) pause(); }
    struct timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
DWORD SleepEx(DWORD ms, BOOL) { Sleep(ms); return 0; }
BOOL GetThreadTimes(HANDLE h, LPFILETIME create, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) {
    const mach_port_t port = thread_port(h);
    if (!port) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS) return FALSE;
    auto put = [](LPFILETIME ft, uint64_t v100) {
        if (!ft) return;
        ft->dwLowDateTime = static_cast<DWORD>(v100);
        ft->dwHighDateTime = static_cast<DWORD>(v100 >> 32);
    };
    put(create, 0);
    put(exit, 0);
    put(kernel, (static_cast<uint64_t>(info.system_time.seconds) * 1000000ull + info.system_time.microseconds) * 10ull);
    put(user, (static_cast<uint64_t>(info.user_time.seconds) * 1000000ull + info.user_time.microseconds) * 10ull);
    return TRUE;
}
BOOL QueryThreadCycleTime(HANDLE h, PULONG64 cycles) {
    FILETIME k, u;
    if (!GetThreadTimes(h, nullptr, nullptr, &k, &u)) return FALSE;
    const uint64_t t100 = ((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                          ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
    *cycles = t100 * 100;   // nanoseconds stand in for cycles
    return TRUE;
}

// ---- events --------------------------------------------------------------------------------------------------------------------------------
namespace {
struct EventObj final : Object {
    bool manual = false, signaled = false;
    bool waitable() const override { return true; }
    bool try_acquire(uint64_t) override {
        if (!signaled) return false;
        if (!manual) signaled = false;
        return true;
    }
};
}  // namespace
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR) {
    auto* e = new EventObj;
    e->manual = manual;
    e->signaled = initial;
    return handle_of(e);
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR) { return CreateEventA(sa, manual, initial, nullptr); }
HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR, DWORD flags, DWORD) {
    return CreateEventA(sa, (flags & CREATE_EVENT_MANUAL_RESET) != 0, (flags & CREATE_EVENT_INITIAL_SET) != 0, nullptr);
}
BOOL SetEvent(HANDLE h) {
    auto* e = as<EventObj>(h);
    if (!e) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::lock_guard<std::mutex> g(e->m);
    e->signaled = true;
    e->signal_locked();
    return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    auto* e = as<EventObj>(h);
    if (!e) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::lock_guard<std::mutex> g(e->m);
    e->signaled = false;
    return TRUE;
}

// ---- mutexes (named ones are also visible to other processes, through lock files in $TMPDIR) ----------------------------------------------
namespace {
struct MutexObj final : Object {
    uint64_t owner = 0;
    int count = 0;
    std::string name;
    int presence_fd = -1, own_fd = -1;   // named: a shared lock on presence_fd while open, an exclusive one on own_fd while owned
    bool waitable() const override { return true; }
    bool try_acquire(uint64_t key) override {
        if (owner == key) { ++count; return true; }
        if (owner) return false;
        if (own_fd >= 0 && flock(own_fd, LOCK_EX | LOCK_NB) != 0) return false;
        owner = key;
        count = 1;
        return true;
    }
    uint64_t deadline_ns() override { return own_fd >= 0 && !owner ? mono_ns() + 10000000ull : UINT64_MAX; }   // poll other processes
    ~MutexObj() override {
        if (own_fd >= 0) close(own_fd);
        if (presence_fd >= 0) close(presence_fd);
    }
};
std::mutex g_named_m;
std::map<std::string, MutexObj*> g_named;
std::string lock_path(const std::string& name, const char* suffix) {
    std::string s;
    for (char c : name) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? c : '_';
    const char* tmp = getenv("TMPDIR");
    std::string dir = tmp && *tmp ? tmp : "/tmp/";
    if (dir.back() != '/') dir += '/';
    return dir + "nxwin." + s + suffix;
}
struct NamedMutexCloser {
    static void close(MutexObj* m) {
        std::lock_guard<std::mutex> g(g_named_m);
        auto it = g_named.find(m->name);
        if (it != g_named.end() && it->second == m) g_named.erase(it);
    }
};
}  // namespace
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL owned, LPCSTR name) {
    if (name && *name) {
        std::lock_guard<std::mutex> g(g_named_m);
        auto it = g_named.find(name);
        if (it != g_named.end()) {
            it->second->ref();
            SetLastError(ERROR_ALREADY_EXISTS);
            return handle_of(it->second);
        }
        auto* m = new MutexObj;
        m->name = name;
        m->presence_fd = open(lock_path(name, ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        m->own_fd = open(lock_path(name, ".own").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        bool exists = false;
        if (m->presence_fd >= 0) {
            exists = flock(m->presence_fd, LOCK_EX | LOCK_NB) != 0;
            flock(m->presence_fd, LOCK_SH);
        }
        g_named[name] = m;
        if (owned) {
            std::lock_guard<std::mutex> mg(m->m);
            m->try_acquire(thread_key());
        }
        SetLastError(exists ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
        return handle_of(m);
    }
    auto* m = new MutexObj;
    if (owned) { m->owner = thread_key(); m->count = 1; }
    SetLastError(ERROR_SUCCESS);
    return handle_of(m);
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCWSTR name) {
    return CreateMutexA(sa, owned, name ? narrow(name).c_str() : nullptr);
}
BOOL ReleaseMutex(HANDLE h) {
    auto* m = as<MutexObj>(h);
    if (!m) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::lock_guard<std::mutex> g(m->m);
    if (m->owner != thread_key()) { SetLastError(ERROR_NOT_OWNER); return FALSE; }
    if (--m->count == 0) {
        m->owner = 0;
        if (m->own_fd >= 0) flock(m->own_fd, LOCK_UN);
        m->signal_locked();
    }
    return TRUE;
}

// ---- waitable timers ------------------------------------------------------------------------------------------------------------------------
namespace {
struct TimerObj final : Object {
    bool manual = false, armed = false, signaled = false;
    uint64_t due = 0;           // mono_ns
    uint64_t period_ns = 0;
    bool waitable() const override { return true; }
    void update() {
        if (armed && !signaled && mono_ns() >= due) {
            signaled = true;
            if (period_ns) {
                due += period_ns;
            } else {
                armed = false;
            }
        }
    }
    bool try_acquire(uint64_t) override {
        update();
        if (!signaled) return false;
        if (!manual) signaled = false;
        return true;
    }
    uint64_t deadline_ns() override { return armed && !signaled ? due : UINT64_MAX; }
};
}  // namespace
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES, BOOL manual, LPCWSTR) {
    auto* t = new TimerObj;
    t->manual = manual;
    return handle_of(t);
}
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR, DWORD flags, DWORD) {
    return CreateWaitableTimerW(sa, (flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0, nullptr);
}
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE, LPVOID, BOOL) {
    auto* t = as<TimerObj>(h);
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const uint64_t now = mono_ns();
    uint64_t at;
    if (due->QuadPart < 0) {
        at = now + static_cast<uint64_t>(-due->QuadPart) * 100ull;
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const int64_t real_now = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        const int64_t delta100 = due->QuadPart - real_now;
        at = delta100 <= 0 ? now : now + static_cast<uint64_t>(delta100) * 100ull;
    }
    std::lock_guard<std::mutex> g(t->m);
    t->due = at;
    t->period_ns = period > 0 ? static_cast<uint64_t>(period) * 1000000ull : 0;
    t->armed = true;
    t->signaled = false;
    t->signal_locked();          // waiters recompute their deadline
    return TRUE;
}
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE apc, LPVOID arg, PREASON_CONTEXT, ULONG) {
    return SetWaitableTimer(h, due, period, apc, arg, FALSE);
}
BOOL CancelWaitableTimer(HANDLE h) {
    auto* t = as<TimerObj>(h);
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::lock_guard<std::mutex> g(t->m);
    t->armed = false;
    return TRUE;
}

// ---- processes ------------------------------------------------------------------------------------------------------------------------------
namespace {
struct ProcObj final : Object {
    pid_t pid = 0;
    bool child = false, done = false;
    DWORD code = STILL_ACTIVE;
    bool waitable() const override { return true; }
    bool try_acquire(uint64_t) override {
        if (done) return true;
        if (child) {
            int st = 0;
            const pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) {
                done = true;
                code = WIFEXITED(st) ? static_cast<DWORD>(WEXITSTATUS(st)) : 0xC0000000u | static_cast<DWORD>(WTERMSIG(st));
            } else if (r < 0 && errno == ECHILD) {
                done = true;
                code = 0;
            }
        } else if (kill(pid, 0) != 0 && errno == ESRCH) {
            done = true;
            code = 0;
        }
        return done;
    }
    uint64_t deadline_ns() override { return done ? UINT64_MAX : mono_ns() + 10000000ull; }
};
// Windows command line to argv (the CommandLineToArgvW rules for quotes; backslashes are kept as they are).
std::vector<std::string> split_cmdline(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false, any = false;
    for (char c : cmd) {
        if (c == '"') { q = !q; any = true; continue; }
        if (!q && (c == ' ' || c == '\t')) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}
}  // namespace
BOOL CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR dir, LPSTARTUPINFOA,
                    LPPROCESS_INFORMATION pi) {
    std::vector<std::string> args = split_cmdline(cmd ? cmd : "");
    std::string exe = app ? fix_path(app) : (args.empty() ? std::string() : fix_path(args[0]));
    if (exe.empty()) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (args.empty()) args.push_back(exe);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (dir && *dir) posix_spawn_file_actions_addchdir_np(&fa, fix_path(dir).c_str());
    pid_t pid = 0;
    const int r = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r != 0) { SetLastError(errno_to_win32(r)); return FALSE; }
    auto* p = new ProcObj;
    p->pid = pid;
    p->child = true;
    if (pi) {
        pi->hProcess = handle_of(p);
        pi->hThread = CreateEventA(nullptr, TRUE, FALSE, nullptr);   // a placeholder the caller may close
        pi->dwProcessId = static_cast<DWORD>(pid);
        pi->dwThreadId = 0;
    } else {
        p->unref();
    }
    return TRUE;
}
BOOL CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
                    LPCWSTR dir, LPSTARTUPINFOW, LPPROCESS_INFORMATION pi) {
    std::string a = narrow(app), c = narrow(cmd), d = narrow(dir);
    return CreateProcessA(app ? a.c_str() : nullptr, cmd ? c.data() : nullptr, pa, ta, inherit, flags, env, dir ? d.c_str() : nullptr, nullptr, pi);
}
HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(kPseudoProcess); }
DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
DWORD GetProcessId(HANDLE h) {
    int pseudo = 0;
    Object* o = resolve(h, &pseudo);
    if (pseudo == kPseudoProcess) return GetCurrentProcessId();
    auto* p = dynamic_cast<ProcObj*>(o);
    return p ? static_cast<DWORD>(p->pid) : 0;
}
HANDLE OpenProcess(DWORD, BOOL, DWORD pid) {
    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    auto* p = new ProcObj;
    p->pid = static_cast<pid_t>(pid);
    return handle_of(p);
}
BOOL TerminateProcess(HANDLE h, UINT code) {
    int pseudo = 0;
    Object* o = resolve(h, &pseudo);
    if (pseudo == kPseudoProcess || (o && dynamic_cast<ProcObj*>(o) && static_cast<ProcObj*>(o)->pid == getpid())) _exit(static_cast<int>(code & 0xFF));
    auto* p = dynamic_cast<ProcObj*>(o);
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return kill(p->pid, SIGKILL) == 0;
}
void ExitProcess(UINT code) { std::exit(static_cast<int>(code & 0xFF)); }
BOOL GetExitCodeProcess(HANDLE h, LPDWORD code) {
    auto* p = as<ProcObj>(h);
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::lock_guard<std::mutex> g(p->m);
    p->try_acquire(0);
    *code = p->done ? p->code : STILL_ACTIVE;
    return TRUE;
}
BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
DWORD GetPriorityClass(HANDLE) { return NORMAL_PRIORITY_CLASS; }
BOOL GetProcessTimes(HANDLE, LPFILETIME create, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    auto put = [](LPFILETIME ft, uint64_t v100) {
        if (!ft) return;
        ft->dwLowDateTime = static_cast<DWORD>(v100);
        ft->dwHighDateTime = static_cast<DWORD>(v100 >> 32);
    };
    put(create, 0);
    put(exit, 0);
    put(kernel, (static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000ull + static_cast<uint64_t>(ru.ru_stime.tv_usec)) * 10ull);
    put(user, (static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000ull + static_cast<uint64_t>(ru.ru_utime.tv_usec)) * 10ull);
    return TRUE;
}
BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask) {
    const DWORD n = GetActiveProcessorCount(0);
    const DWORD_PTR m = n >= 64 ? ~DWORD_PTR{0} : (DWORD_PTR{1} << n) - 1;
    if (proc_mask) *proc_mask = m;
    if (sys_mask) *sys_mask = m;
    return TRUE;
}
BOOL GetProcessHandleCount(HANDLE, PDWORD count) {
    int n = 0;
    if (DIR* d = opendir("/dev/fd")) {
        while (readdir(d)) ++n;
        closedir(d);
    }
    *count = static_cast<DWORD>(n);
    return TRUE;
}
DWORD GetGuiResources(HANDLE, DWORD) { return 0; }

// ---- handles and waits ---------------------------------------------------------------------------------------------------------------------
BOOL CloseHandle(HANDLE h) {
    int pseudo = 0;
    Object* o = resolve(h, &pseudo);
    if (pseudo) return TRUE;
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (auto* m = dynamic_cast<MutexObj*>(o); m && !m->name.empty() && o->refs.load() == 1) NamedMutexCloser::close(m);
    o->on_close();
    o->unref();
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, LPHANDLE out, DWORD, BOOL, DWORD options) {
    int pseudo = 0;
    Object* o = resolve(src, &pseudo);
    if (pseudo == kPseudoThread) o = current_thread_obj();
    if (pseudo == kPseudoProcess) {
        auto* p = new ProcObj;
        p->pid = getpid();
        *out = handle_of(p);
        return TRUE;
    }
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    o->ref();
    *out = handle_of(o);
    if (options & DUPLICATE_CLOSE_SOURCE) CloseHandle(src);
    return TRUE;
}

DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL) {
    if (n == 0 || n > MAXIMUM_WAIT_OBJECTS) { SetLastError(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    Object* objs[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < n; ++i) {
        int pseudo = 0;
        objs[i] = resolve(hs[i], &pseudo);
        if (pseudo == kPseudoThread) objs[i] = current_thread_obj();      // waiting on oneself never ends
        if (!objs[i] && !pseudo) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
        if (objs[i]) objs[i]->ref();
    }
    struct Unref {
        Object** o;
        DWORD n;
        ~Unref() { for (DWORD i = 0; i < n; ++i) if (o[i]) o[i]->unref(); }
    } unref{objs, n};

    const uint64_t key = thread_key();
    const uint64_t deadline = ms == INFINITE ? UINT64_MAX : mono_ns() + static_cast<uint64_t>(ms) * 1000000ull;
    if (all) {
        // Not atomic across the objects (the runtime does not use wait-all); each is waited for in turn within the timeout.
        for (DWORD i = 0; i < n; ++i) {
            if (!objs[i]) continue;
            const uint64_t now = mono_ns();
            const DWORD left = deadline == UINT64_MAX ? INFINITE : now >= deadline ? 0 : static_cast<DWORD>((deadline - now) / 1000000ull);
            if (WaitForMultipleObjectsEx(1, &hs[i], FALSE, left, FALSE) != WAIT_OBJECT_0) return WAIT_TIMEOUT;
        }
        return WAIT_OBJECT_0;
    }

    Waiter w;
    for (;;) {
        DWORD registered = 0;
        uint64_t wake_at = deadline;
        for (DWORD i = 0; i < n; ++i) {
            Object* o = objs[i];
            if (!o) continue;
            std::lock_guard<std::mutex> g(o->m);
            if (o->try_acquire(key)) {
                for (DWORD j = 0; j < i; ++j) {
                    if (!objs[j]) continue;
                    std::lock_guard<std::mutex> g2(objs[j]->m);
                    auto& v = objs[j]->waiters;
                    v.erase(std::remove(v.begin(), v.end(), &w), v.end());
                }
                return WAIT_OBJECT_0 + i;
            }
            o->waiters.push_back(&w);
            wake_at = std::min(wake_at, o->deadline_ns());
            ++registered;
        }
        (void)registered;
        const uint64_t now = mono_ns();
        bool timed_out = now >= deadline;
        if (!timed_out) {
            std::unique_lock<std::mutex> lk(w.m);
            while (!w.woke) {
                const uint64_t t = mono_ns();
                if (t >= wake_at) break;
                const uint64_t left = wake_at - t;
                // Short waits on a timer alone: sleep to the due time with mach_wait_until (precise), not a condition variable.
                if (n == 1 && wake_at < deadline && left < 2000000ull) {
                    lk.unlock();
                    mach_timebase_info_data_t tb;
                    mach_timebase_info(&tb);
                    mach_wait_until(mach_absolute_time() + left * tb.denom / tb.numer);
                    lk.lock();
                    break;
                }
                w.cv.wait_for(lk, std::chrono::nanoseconds(left));
            }
            w.woke = false;
        }
        for (DWORD i = 0; i < n; ++i) {
            if (!objs[i]) continue;
            std::lock_guard<std::mutex> g(objs[i]->m);
            auto& v = objs[i]->waiters;
            v.erase(std::remove(v.begin(), v.end(), &w), v.end());
        }
        if (timed_out || mono_ns() >= deadline) {
            // One last check: an object signaled right at the deadline still counts.
            for (DWORD i = 0; i < n; ++i) {
                if (!objs[i]) continue;
                std::lock_guard<std::mutex> g(objs[i]->m);
                if (objs[i]->try_acquire(key)) return WAIT_OBJECT_0 + i;
            }
            return WAIT_TIMEOUT;
        }
    }
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) { return WaitForMultipleObjectsEx(n, hs, all, ms, FALSE); }
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjectsEx(1, &h, FALSE, ms, FALSE); }
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) { return WaitForMultipleObjectsEx(1, &h, FALSE, ms, alertable); }

// ---- thread snapshots ------------------------------------------------------------------------------------------------------------------------
namespace {
struct SnapObj final : Object {
    std::vector<DWORD> tids;
    size_t pos = 0;
};
}  // namespace
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD) {
    auto* s = new SnapObj;
    if (flags & TH32CS_SNAPTHREAD) {
        thread_act_array_t list;
        mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
            for (mach_msg_type_number_t i = 0; i < n; ++i) {
                s->tids.push_back(static_cast<DWORD>(list[i]));
                mach_port_deallocate(mach_task_self(), list[i]);
            }
            vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
        }
    }
    return handle_of(s);
}
static BOOL thread_entry_at(HANDLE snap, LPTHREADENTRY32 te) {
    auto* s = as<SnapObj>(snap);
    if (!s || s->pos >= s->tids.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    te->cntUsage = 1;
    te->th32ThreadID = s->tids[s->pos++];
    te->th32OwnerProcessID = static_cast<DWORD>(getpid());
    te->tpBasePri = 8;
    te->tpDeltaPri = 0;
    te->dwFlags = 0;
    return TRUE;
}
BOOL Thread32First(HANDLE snap, LPTHREADENTRY32 te) {
    if (auto* s = as<SnapObj>(snap)) s->pos = 0;
    return thread_entry_at(snap, te);
}
BOOL Thread32Next(HANDLE snap, LPTHREADENTRY32 te) { return thread_entry_at(snap, te); }

// ---- MMCSS ------------------------------------------------------------------------------------------------------------------------------------
HANDLE AvSetMmThreadCharacteristicsA(LPCSTR, LPDWORD index) {
    if (index) *index = 1;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return reinterpret_cast<HANDLE>(static_cast<intptr_t>(0x4D4D4353));   // non-null token, never dereferenced
}
HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR, LPDWORD index) { return AvSetMmThreadCharacteristicsA(nullptr, index); }
BOOL AvRevertMmThreadCharacteristics(HANDLE) { return TRUE; }
BOOL AvSetMmThreadPriority(HANDLE, AVRT_PRIORITY) { return TRUE; }
