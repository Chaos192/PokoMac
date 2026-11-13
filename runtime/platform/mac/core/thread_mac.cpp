// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Chaos192
// macOS: scheduling for the threads a frame waits on. On Apple silicon a thread's QoS decides whether it runs on the performance or the
// efficiency cores, and a real-time (time-constraint) thread is scheduled ahead of every ordinary one, as Core Audio's and the display's
// are. The NVN render thread (replay and present) gets both, so a frame's GPU work is never late because its thread sat on an efficiency
// core or behind the game's own threads. [gfx] render_thread_policy: realtime (default), interactive (QoS only) or default (left alone).
// The kernel takes the real-time policy back for a while from a thread that keeps running past its budget, so a long upload cannot
// starve the rest of the system.
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <string>

namespace nxrt::platform {

void mac_realtime_thread(const char* what, double period_ms, double budget_ms) {
    const std::string mode = config_get_str("gfx", "render_thread_policy", "realtime");
    if (mode == "default") return;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (mode != "realtime") {
        NXLOG_I("gfx", "%s thread: user-interactive QoS ([gfx] render_thread_policy = %s)", what, mode.c_str());
        return;
    }
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    auto ticks = [&](double ms) { return static_cast<uint32_t>(ms * 1e6 * tb.denom / tb.numer); };
    thread_time_constraint_policy_data_t p{};
    p.period = ticks(period_ms);
    p.computation = ticks(budget_ms);
    p.constraint = ticks(period_ms);
    p.preemptible = TRUE;
    const kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                               reinterpret_cast<thread_policy_t>(&p), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS)
        NXLOG_I("gfx", "%s thread: real-time (period %.2f ms, budget %.2f ms) on the performance cores", what, period_ms, budget_ms);
    else
        NXLOG_W("gfx", "%s thread: the real-time policy was refused (%d); user-interactive QoS only", what, static_cast<int>(kr));
}

}  // namespace nxrt::platform
