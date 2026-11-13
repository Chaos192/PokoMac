// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Thread registry and recent-file ring (declared in nxrt/diag.h).
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "nxrt/diag.h"

namespace nxrt {
namespace {

std::mutex g_lock;
std::vector<ThreadRecord> g_threads;
thread_local ::Ctx* t_ctx = nullptr;
thread_local size_t t_index = ~size_t(0);

constexpr size_t kRing = 32;
std::mutex g_files_lock;
std::string g_files[kRing];
size_t g_files_next = 0, g_files_count = 0;

}  // namespace

size_t diag_register_thread(const char* name, ::Ctx* ctx, uint64_t guest_tid) {
    HANDLE real = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    ThreadRecord r;
    r.name = name ? name : "";
    r.host_tid = GetCurrentThreadId();
    r.host_handle = real;
    r.ctx = ctx;
    r.guest_tid = guest_tid;
    t_ctx = ctx;
    std::lock_guard<std::mutex> lk(g_lock);
    // reuse a free slot (host_tid == 0)
    for (size_t i = 0; i < g_threads.size(); i++)
        if (g_threads[i].host_tid == 0) {
            g_threads[i] = r;
            t_index = i;
            return i;
        }
    g_threads.push_back(r);
    t_index = g_threads.size() - 1;
    return t_index;
}

void diag_unregister_thread() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (t_index < g_threads.size() && g_threads[t_index].host_tid == GetCurrentThreadId()) {
        if (g_threads[t_index].host_handle) CloseHandle(g_threads[t_index].host_handle);
        g_threads[t_index] = ThreadRecord{};
    }
    t_index = ~size_t(0);
    t_ctx = nullptr;
}

void diag_set_thread_ctx(::Ctx* ctx) {
    t_ctx = ctx;
    if (t_index != ~size_t(0)) {
        std::lock_guard<std::mutex> lk(g_lock);
        if (t_index < g_threads.size()) g_threads[t_index].ctx = ctx;
    }
}

::Ctx* diag_thread_ctx() { return t_ctx; }

std::vector<ThreadRecord> diag_threads() {
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<ThreadRecord> out;
    for (auto& t : g_threads) {
        if (!t.host_tid) continue;
        out.push_back(t);
        out.back().host_handle = nullptr;   // the registry's handle can be closed by the owning thread at any time: never hand it out
    }
    return out;
}

std::vector<ThreadRecord> diag_threads_dup() {
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<ThreadRecord> out;
    for (auto& t : g_threads) {
        if (!t.host_tid) continue;
        ThreadRecord r = t;
        r.host_handle = nullptr;
        if (t.host_handle) {
            HANDLE d = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(t.host_handle), GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) d = nullptr;
            r.host_handle = d;
        }
        out.push_back(std::move(r));
    }
    return out;
}

void diag_threads_release(std::vector<ThreadRecord>& recs) {
    for (ThreadRecord& r : recs)
        if (r.host_handle) CloseHandle(static_cast<HANDLE>(r.host_handle));
    recs.clear();
}

void diag_note_file(const char* what, const char* path) {
    char buf[600];
    snprintf(buf, sizeof buf, "[%lu] %s %s", GetCurrentThreadId(), what ? what : "?", path ? path : "(null)");
    std::lock_guard<std::mutex> lk(g_files_lock);
    g_files[g_files_next] = buf;
    g_files_next = (g_files_next + 1) % kRing;
    if (g_files_count < kRing) g_files_count++;
}

std::vector<std::string> diag_recent_files() {
    std::lock_guard<std::mutex> lk(g_files_lock);
    std::vector<std::string> out;
    size_t start = (g_files_next + kRing - g_files_count) % kRing;
    for (size_t i = 0; i < g_files_count; i++) out.push_back(g_files[(start + i) % kRing]);
    return out;
}

void diag_clear_recent_files() {
    std::lock_guard<std::mutex> lk(g_files_lock);
    g_files_next = g_files_count = 0;
}

}  // namespace nxrt
