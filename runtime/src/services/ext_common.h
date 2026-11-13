// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Helpers for the command extensions (ext_*.cpp): commands added to interfaces by name (ipc::extend, include/nxrt/ipc.h) for what titles
// beyond the first port call. Each answer is what a console with no network and no extra hardware gives: the layout comes from Lichii's
// handler (tools/services/reference.json lists it) and Switchbrew; a command that cannot be answered properly yet returns a real Horizon
// result of its module, logged once by name, never an output of the wrong size.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/ipc.h"
#include "nxrt/log.h"
#include "nxrt/services.h"

namespace nxrt::services::ext {

using ipc::Request;
using ipc::Response;
using ipc::Service;

// Logs `what` once (per interface and command) at Info with the answer; later calls are silent.
void note_once(const std::string& iface, uint32_t id, const char* what, uint32_t result);

// A handler for `iface` cmd `id`: logs its first use, then runs `fn`.
template <typename F>
void add(const char* iface, uint32_t id, const char* name, F fn) {
    const std::string i = iface;
    ipc::extend(i, id, name, [i, id, name, fn](Service& s, Request& req, Response& rsp) -> uint32_t {
        const uint32_t r = fn(s, req, rsp);
        note_once(i, id, name, r);
        return r;
    });
}

// Accepted and ignored: result 0, `zero_bytes` of zero output.
void ok(const char* iface, uint32_t id, const char* name, size_t zero_bytes = 0);
// Result 0 and one value of type T.
template <typename T>
void value(const char* iface, uint32_t id, const char* name, T v) {
    add(iface, id, name, [v](Service&, Request&, Response& rsp) -> uint32_t {
        rsp.push(v);
        return 0;
    });
}
// Not available on this console (no network, no such hardware): result `rc`, no output, logged once by name at Warn.
void unavailable(const char* iface, uint32_t id, const char* name, uint32_t rc);
// Result 0, an event that is never signalled (a copy handle).
void event(const char* iface, uint32_t id, const char* name, bool signalled = false);
// Result 0, zeros written over the whole first output buffer, `zero_bytes` of zero raw output.
void zero_buffer(const char* iface, uint32_t id, const char* name, size_t zero_bytes = 0);

// Result codes used by more than one extension file (module | description << 9; Switchbrew "Error codes", Lichii's *_results.h).
constexpr uint32_t kNifmNoInternet = res(110, 300);   // nifm: the network is not available (what every online request ends in here)

// Registration of each extension file (ext_*.cpp), called once from register_extensions().
void register_account();
void register_system();
void register_hid_extra();
void register_new_services();
void register_am();
void register_ns_news();
void register_sound_mixer();

// auddmg:u's AttachStream / DetachStream (nn::audio's StartOutputStream / StopOutputStream): engage or disengage the sound mixer stream
// `id` (ext_audsmx.cpp) on the virtual device of `functionality` (1 output, 2 input). Returns the Horizon result.
uint32_t sound_mixer_attach(uint32_t id, uint32_t functionality, bool attach);

}  // namespace nxrt::services::ext
