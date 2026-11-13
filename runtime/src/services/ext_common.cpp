// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The command extensions' helpers (ext_common.h) and their registration: register_extensions() is called by services::register_all().
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "audio_opus.h"
#include "ext_common.h"
#include "services_internal.h"

namespace nxrt::services {

namespace ext {

namespace {
std::mutex g_mu;
std::set<std::pair<std::string, uint32_t>>& seen() {
    static std::set<std::pair<std::string, uint32_t>> s;
    return s;
}
}  // namespace

void note_once(const std::string& iface, uint32_t id, const char* what, uint32_t result) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!seen().insert({iface, id}).second) return;
    }
    if (result == 0)
        NXLOG_I("svc", "%s cmd %u %s -> 0 (first use)", iface.c_str(), id, what);
    else
        NXLOG_W("svc", "%s cmd %u %s -> 0x%X: not available on this console (first use)", iface.c_str(), id, what, result);
}

void ok(const char* iface, uint32_t id, const char* name, size_t zero_bytes) {
    add(iface, id, name, [zero_bytes](Service&, Request&, Response& rsp) -> uint32_t {
        rsp.out.insert(rsp.out.end(), zero_bytes, uint8_t{0});
        return 0;
    });
}

void unavailable(const char* iface, uint32_t id, const char* name, uint32_t rc) {
    add(iface, id, name, [rc](Service&, Request&, Response&) -> uint32_t { return rc; });
}

void event(const char* iface, uint32_t id, const char* name, bool signalled) {
    add(iface, id, name, [signalled](Service&, Request&, Response& rsp) -> uint32_t {
        ServiceEvent ev(signalled);   // the guest's handle keeps the event alive
        rsp.out_copy.push_back(ev.new_handle());
        return 0;
    });
}

void zero_buffer(const char* iface, uint32_t id, const char* name, size_t zero_bytes) {
    add(iface, id, name, [zero_bytes](Service&, Request& req, Response& rsp) -> uint32_t {
        const ipc::Buffer b = req.out_buf(0);
        if (b.addr && b.size) std::memset(buf_mut(b), 0, static_cast<size_t>(b.size));
        rsp.out.insert(rsp.out.end(), zero_bytes, uint8_t{0});
        return 0;
    });
}

}  // namespace ext

void register_extensions() {
    // PokoMac: the extensions of the services it has taken from AnySwitch so far (the Switch 2 sound mixer for audio.cpp, the hardware
    // audio codec for audio_opus.cpp); AnySwitch's others (ext_account, ext_system, ext_hid, ext_services, ext_am, ext_ns) come with
    // their services.
    ext::register_sound_mixer();
    opus::register_opus_codec();
}

void extensions_reset() {
    std::lock_guard<std::mutex> lk(ext::g_mu);
    ext::seen().clear();
}

}  // namespace nxrt::services
