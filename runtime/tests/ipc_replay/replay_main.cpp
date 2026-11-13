// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// nxrt_ipc_replay: replays a request script against the service library and writes every answer, so two builds of a service (an old
// and a new implementation) can be compared on the same requests (tools/services/ipc_diff/ipc_diff.py builds both and compares).
//
//   nxrt_ipc_replay --script <file> --out <file> [--tmp <folder>] [--timeout <seconds>]
//
// The script is the recorder's format (src/ipc/record.cpp, [ipc] record: a game's run) or a generated one in the same format, plus:
//   reset                              services::reset_all(); every service object of the script is dropped (no on_close)
//   reset keep                         services::reset_all(); the script's service objects stay (as sessions a guest still holds)
//   config <section> <key> <value>     nxrt::config_set, then services::profile_reload()
//   # ...                              a comment
//   api <what> ...                     a C++ entry point of a service file (state a request cannot reach), answered with an "A" line:
//     api hle <name> <x0> <x1>           the HLE handler registered as <name> (hle_lookup), x0 / x1: null, u32:<hex> or bytes:<hex> (a
//                                        pointer to those bytes); prints x0 and x1 after the call
//     api lang_hooks                     services::install_language_compare_hooks()
//     api accuid                         accapi::user_uid
//     api face_set <style> <names>       shared_font_set_face (names: 18 comma-separated prompt names, "-" for an empty one)
//     api face_get                       shared_font_face
//     api face_provider                  shared_font_set_face_provider with a fixed face (style "xbox")
//     api vi_present                     victl::note_present (a presented frame)
//     api vi_frames                      victl::frames_presented
//     api vi_history <max>               victl::frame_history (status and frame number of each entry; how its times relate)
//     api vi_vsync                       viapi::display_vsync_handle (whether a handle came back)
//     api am_docked <0|1>                amctl::set_docked
//     api am_focus <0|1>                 amctl::set_focus
//     api am_applet <0|1>                amctl::applet_open (a library applet answered on the host opens / closes)
//     api am_state                       amctl::docked and amctl::in_focus
//     api am_exit_hook                   amctl::set_exit_hook: a hand-off's exit becomes an "H exit <code> <why>" line, the call returns
//     api file <path>                    a file a service wrote (the hand-off): its size and bytes (a hash above 4 KiB), or none
//     api file_rm <path>                 removes that file
//     api sleep_ms <n>                   waits n ms (a state that ages: the Joy-Con 2 mouse feed)
//     api tmem                           every transfer memory the script made (hm=t / hc=t): its size and contents hash
//   hid (hid.cpp; the sampler thread samples nothing while background sampling is off, so the shared memory changes only on hid_sample):
//     api hid_bg <0|1>                   hid_set_background_sampling (reset turns it on again: send hid_bg 0 after every reset)
//     api hid_sample [n]                 hid_sample_once, n times (default 1)
//     api hid_shm                        hid_shared_memory_for_tests: the block's hash, or none
//     api hid_live                       hid_sampler_live
//     api hid_mode_changed               hid_mode_changed
//     api hid_signal <index>             hid_signal_style_event
//     api hid_sets                       hid_supported_sets and nxrt::hid::supported_player_mask
//     api hid_read | hid_raw             hid_read_input | hid_read_raw_input: every connected slot, touch, keyboard, mouse, sampled_buttons
//     api hid_src none|manual            the input source: none, or a fresh scripted one (nothing connected) the lines below change
//     api hid_pad <slot> <connected> <style> <buttons hex> <lx> <ly> <rx> <ry>
//     api hid_six <slot> <at_rest> <accel x y z> <gyro x y z> <angle x y z> [<direction 9 floats>]
//     api hid_color <slot> <body hex> <buttons hex>
//     api hid_touch <count> [<id>:<x>:<y>:<dx>:<dy>:<angle> ...]
//     api hid_kbd <connected> <modifiers hex> <keys0..3 hex>
//     api hid_mouse <connected> <x> <y> <dx> <dy> <wheel dx> <wheel dy> <buttons hex>
//     api hid_vib_sink <0|1>             a vibration sink: every value hid forwards is a "V <slot> <hand> <4 floats as hex bits>" line
//     api hid_bench_open <max>           hid_applet_open (hid_sdk.cpp: the controller applet opens)
//     api hid_bench_close <max>          hid_applet_close: the players, player 1's npad id, the bench mask
//     api hid_bench                      hid_bench_mask
//     api web_hold <0|1>                 webview::set_input_held, with a host whose pad() is a "W pad <buttons> <lx> <ly>" line (0: no host)
//     api jc2_mode off|auto|on           nxrt::hid::jc2_mouse_set_mode
//     api jc2_feed <u> <v> <buttons> <wheel x> <wheel y> <focused> <captured>   nxrt::hid::jc2_mouse_feed
//     api jc2_request <0|1>              nxrt::hid::jc2_mouse_request
//     api jc2_space <w> <h>              nxrt::hid::jc2_mouse_set_pointer_space
//     api jc2_state                      the mode's name, jc2_mouse_wants_pointer, jc2_mouse_active, jc2_mouse_listed
// In a Q line's raw data and input buffers the 8 bytes 4b4b4b4b4b4b4b4b ("KKKKKKKK") stand for the address of a 1 MiB sample area
// (filled with a fixed pattern at start), so a generated request can point at guest memory (an AudioOutBuffer's samples).
// A Q line may end in as=<sid>,<sid> to name the objects its reply returns (generated scripts); a recorded script names them in its A
// line (o=<sid>:<iface>). A request to a sid no earlier reply returned goes to make_service(<iface>) when that is "sm:" or a registered
// name, else it is skipped. Requests are handed to the service object directly (Service::handle), one at a time, on this thread.
//
// Output, one line per request (only what the guest would see):
//   R <seq> <iface> <cmd> <result> out=<hex> [hc=..] [hm=..] [o=<iface>,..] [b<n>=..] [c<n>=..] [w<n>=..] [ev=<bits>] [shm=<n>:<hash>,..]
// out, the handles and the objects only for result 0 (a failed reply carries none). A handle is e<n> (the n-th distinct event, ":1" when
// signaled), s<n>:<size>:<hash> (shared memory and its contents), t<size>, S:<iface>, p, T or u. Output buffers (filled with 0xCD
// before the call) in hex up to 4 KiB, else #<FNV-1a 64>:<size>. ev= gives every event seen so far, 0/1, when one of them changed;
// shm= the shared memories whose contents changed since the last line.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../src/services/hid_sdk.h"
#include "../../src/services/hid_shm.h"
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/input_source.h"
#include "nxrt/ipc.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "nxrt/shared_font.h"
#include "nxrt/webview.h"

namespace {

namespace ipc = nxrt::ipc;
namespace kernel = nxrt::kernel;

FILE* g_out = nullptr;
std::atomic<uint64_t> g_running_seq{0};
std::atomic<int64_t> g_call_started_ms{-1};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t fnv(const uint8_t* p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void appendf(std::string& s, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    s += b;
}

void hex_to(std::string& s, const uint8_t* p, uint64_t n) {
    static const char* d = "0123456789abcdef";
    if (!p || !n) {
        s += '-';
        return;
    }
    for (uint64_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
}

std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> v;
    if (h == "-" || h.empty() || h[0] == '#') return v;
    auto nib = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; };
    v.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back(static_cast<uint8_t>(nib(h[i]) << 4 | nib(h[i + 1])));
    return v;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) {
            v.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    v.push_back(cur);
    return v;
}

// Guest memory for the buffers: the kernel region, as the SDK's buffers are guest memory (below 2^39 as the descriptors require).
struct Arena {
    uint8_t* base = nullptr;
    uint64_t cap = 0, used = 0;
    uint8_t* take(uint64_t n) {
        if (!base) {
            cap = 192ull << 20;
            base = reinterpret_cast<uint8_t*>(nxrt::mem::alloc_kernel(cap));
        }
        if (!base || used + n > cap) return nullptr;
        uint8_t* p = base + used;
        used = (used + n + 15) & ~uint64_t{15};
        return p;
    }
};
Arena g_req_arena;    // reset for every request
Arena g_keep_arena;   // transfer memories (the service keeps using them)
uint8_t* g_sample_area = nullptr;
constexpr uint64_t kSampleAreaBytes = 1u << 20;

// "KKKKKKKK" in request bytes becomes the sample area's address.
void place_sample_area(std::vector<uint8_t>& v) {
    if (!g_sample_area) {
        g_sample_area = g_keep_arena.take(kSampleAreaBytes);
        if (g_sample_area)
            for (uint64_t i = 0; i < kSampleAreaBytes; ++i) g_sample_area[i] = static_cast<uint8_t>((i * 37 + (i >> 9)) & 0xFF);
    }
    const uint64_t a = reinterpret_cast<uint64_t>(g_sample_area);
    for (size_t i = 0; i + 8 <= v.size(); ++i)
        if (std::memcmp(v.data() + i, "KKKKKKKK", 8) == 0) std::memcpy(v.data() + i, &a, 8);
}

struct State {
    std::unordered_map<uint32_t, std::shared_ptr<ipc::Service>> sids;
    std::map<const kernel::EventState*, int> events;   // ordinal of each distinct event state seen in a reply
    std::vector<std::shared_ptr<kernel::EventState>> event_list;
    std::string last_ev;
    std::map<const kernel::KSharedMemory*, int> shms;
    std::vector<std::shared_ptr<kernel::KSharedMemory>> shm_list;
    std::vector<uint64_t> shm_hash;
    std::vector<std::vector<uint8_t>> shm_copy;   // the contents last reported (a compare is much cheaper than a hash)
    std::vector<std::shared_ptr<kernel::KTransferMemory>> tmems;   // every transfer memory the script made (api tmem)
};
State g;

void out_handle(std::string& s, uint32_t h) {
    auto o = kernel::handle_get(h);
    if (!o) {
        s += 'u';
        return;
    }
    switch (o->type()) {
        case kernel::ObjType::Event: {
            auto e = std::static_pointer_cast<kernel::KEvent>(o);
            auto it = g.events.find(e->state.get());
            int n;
            if (it == g.events.end()) {
                n = static_cast<int>(g.event_list.size());
                g.events[e->state.get()] = n;
                g.event_list.push_back(e->state);
            } else {
                n = it->second;
            }
            appendf(s, "e%d%s", n, e->state->signaled ? ":1" : "");
            break;
        }
        case kernel::ObjType::SharedMemory: {
            auto m = std::static_pointer_cast<kernel::KSharedMemory>(o);
            auto it = g.shms.find(m.get());
            int n;
            const uint64_t hsh = m->host ? fnv(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(m->host)), m->size) : 0;
            if (it == g.shms.end()) {
                n = static_cast<int>(g.shm_list.size());
                g.shms[m.get()] = n;
                g.shm_list.push_back(m);
                g.shm_hash.push_back(hsh);
                g.shm_copy.emplace_back();
            } else {
                n = it->second;
                g.shm_hash[n] = hsh;
            }
            if (m->host) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(m->host));
                g.shm_copy[n].assign(p, p + m->size);
            }
            appendf(s, "s%d:%llx:%016llx", n, static_cast<unsigned long long>(m->size), static_cast<unsigned long long>(hsh));
            break;
        }
        case kernel::ObjType::TransferMemory:
            appendf(s, "t%llx", static_cast<unsigned long long>(std::static_pointer_cast<kernel::KTransferMemory>(o)->size));
            break;
        case kernel::ObjType::Session: {
            auto k = std::static_pointer_cast<ipc::KSession>(o);
            s += "S:";
            s += k->svc ? k->svc->name() : std::string("-");
            break;
        }
        case kernel::ObjType::Process: s += 'p'; break;
        case kernel::ObjType::Thread: s += 'T'; break;
        default: s += 'u'; break;
    }
}

void out_handles(std::string& s, const char* key, const std::vector<uint32_t>& v) {
    if (v.empty()) return;
    s += ' ';
    s += key;
    s += '=';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        out_handle(s, v[i]);
    }
}

void out_bufs(std::string& s, char kind, const std::vector<ipc::Buffer>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        appendf(s, " %c%zu=", kind, i);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(v[i].addr));
        if (!p || !v[i].size) s += '-';
        else if (v[i].size <= 4096) hex_to(s, p, v[i].size);
        else appendf(s, "#%016llx:%llx", static_cast<unsigned long long>(fnv(p, v[i].size)), static_cast<unsigned long long>(v[i].size));
    }
}

// A handle of the kind the recorded request carried, made fresh for the replay.
uint32_t make_handle(const std::string& d, std::vector<std::shared_ptr<kernel::KObject>>& keep) {
    if (d.empty()) return 0;
    switch (d[0]) {
        case 'e': {
            auto e = std::make_shared<kernel::KEvent>();
            keep.push_back(e);
            return kernel::handle_add(e);
        }
        case 'p': return kernel::kPseudoCurrentProcess;
        case 'T': return kernel::kPseudoCurrentThread;
        case 's': {
            const uint64_t size = std::strtoull(d.c_str() + 1, nullptr, 16);
            nxrt::services::ServiceSharedMemory m(size ? size : 0x1000, "replay");
            return m.new_handle();
        }
        case 't': {
            const uint64_t size = std::strtoull(d.c_str() + 1, nullptr, 16);
            auto t = std::make_shared<kernel::KTransferMemory>();
            t->size = size;
            uint8_t* p = g_keep_arena.take(size ? size : 0x1000);
            if (p) std::memset(p, 0, size);
            t->addr = reinterpret_cast<uint64_t>(p);
            keep.push_back(t);
            g.tmems.push_back(t);
            return kernel::handle_add(t);
        }
        case 'S': {
            auto it = g.sids.find(static_cast<uint32_t>(std::strtoul(d.c_str() + 1, nullptr, 10)));
            return it == g.sids.end() ? 0 : ipc::create_session_handle(it->second);
        }
        default: return 0;
    }
}

struct Q {
    uint64_t seq = 0;
    uint32_t sid = 0, cmd = 0, type = 0;
    uint64_t pid = 0;
    std::string iface;
    std::vector<uint8_t> in;
    struct InBuf { char kind; uint64_t size; std::vector<uint8_t> data; };
    std::vector<InBuf> inbufs;                 // a / x / w in order of appearance
    std::vector<std::pair<char, uint64_t>> outbufs;   // b / c
    std::vector<std::string> hc, hm;
    std::vector<uint32_t> os, as;
};

bool parse_q(const std::string& line, Q& q) {
    std::istringstream is(line);
    std::string tag, thread;
    is >> tag >> q.seq >> thread >> q.sid >> q.iface >> q.cmd >> q.type;
    std::string pid;
    is >> pid;
    q.pid = std::strtoull(pid.c_str(), nullptr, 16);
    if (!is) return false;
    std::string tok;
    while (is >> tok) {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (k == "in") {
            q.in = unhex(v);
            place_sample_area(q.in);
        } else if ((k[0] == 'a' || k[0] == 'x' || k[0] == 'w') && k.size() > 1 && isdigit(static_cast<unsigned char>(k[1]))) {
            const size_t c = v.find(':');
            Q::InBuf b{k[0], std::strtoull(v.substr(0, c).c_str(), nullptr, 16), {}};
            if (c != std::string::npos) b.data = unhex(v.substr(c + 1));
            place_sample_area(b.data);
            q.inbufs.push_back(std::move(b));
        } else if ((k[0] == 'b' || k[0] == 'c') && k.size() > 1 && isdigit(static_cast<unsigned char>(k[1]))) {
            q.outbufs.push_back({k[0], std::strtoull(v.c_str(), nullptr, 16)});
        } else if (k == "hc") {
            q.hc = split(v, ',');
        } else if (k == "hm") {
            q.hm = split(v, ',');
        } else if (k == "os") {
            for (const std::string& x : split(v, ',')) q.os.push_back(static_cast<uint32_t>(std::strtoul(x.c_str(), nullptr, 10)));
        } else if (k == "as") {
            for (const std::string& x : split(v, ',')) q.as.push_back(static_cast<uint32_t>(std::strtoul(x.c_str(), nullptr, 10)));
        }
    }
    return true;
}

void emit(const std::string& s) {
    std::fputs(s.c_str(), g_out);
    std::fputc('\n', g_out);
    std::fflush(g_out);
}

void state_tail(std::string& s);

void replay_q(const Q& q, const std::vector<uint32_t>& reply_sids) {
    std::shared_ptr<ipc::Service> svc;
    auto it = g.sids.find(q.sid);
    if (it != g.sids.end()) {
        svc = it->second;
    } else if (q.iface == "sm:" || ipc::has_service(q.iface)) {
        svc = ipc::make_service(q.iface);
        g.sids[q.sid] = svc;
    }
    std::string s;
    appendf(s, "R %llu %s %u ", static_cast<unsigned long long>(q.seq), q.iface.c_str(), q.cmd);
    if (!svc) {
        s += "skip";
        emit(s);
        return;
    }
    g_req_arena.used = 0;
    std::vector<std::shared_ptr<kernel::KObject>> keep;
    ipc::Request req;
    req.cmd = q.cmd;
    req.msg_type = static_cast<uint16_t>(q.type);
    req.tipc = q.type >= 16;
    req.in = q.in.data();
    req.in_size = q.in.size();
    req.pid = q.pid;
    for (const Q::InBuf& b : q.inbufs) {
        uint8_t* p = b.size ? g_req_arena.take(b.size) : nullptr;
        if (p) {
            std::memset(p, 0, b.size);
            std::memcpy(p, b.data.data(), std::min<uint64_t>(b.size, b.data.size()));
        }
        ipc::Buffer buf{reinterpret_cast<uint64_t>(p), p ? b.size : 0};
        (b.kind == 'a' ? req.a : b.kind == 'x' ? req.x : req.w).push_back(buf);
    }
    for (const auto& b : q.outbufs) {
        uint8_t* p = b.second ? g_req_arena.take(b.second) : nullptr;
        if (p) std::memset(p, 0xCD, b.second);
        ipc::Buffer buf{reinterpret_cast<uint64_t>(p), p ? b.second : 0};
        (b.first == 'b' ? req.b : req.c).push_back(buf);
    }
    for (const std::string& d : q.hc) req.in_copy.push_back(make_handle(d, keep));
    for (const std::string& d : q.hm) req.in_move.push_back(make_handle(d, keep));
    for (uint32_t o : q.os) {
        auto f = g.sids.find(o);
        req.in_services.push_back(f == g.sids.end() ? nullptr : f->second);
    }

    ipc::Response rsp;
    uint32_t result;
    g_running_seq.store(q.seq);
    g_call_started_ms.store(now_ms());
    try {
        result = svc->handle(req, rsp);
    } catch (...) {
        rsp = ipc::Response{};
        result = kernel::kInvalidState;
    }
    g_call_started_ms.store(-1);
    for (uint32_t h : req.in_move)
        if (h && h != kernel::kPseudoCurrentProcess && h != kernel::kPseudoCurrentThread) kernel::handle_close(h);

    appendf(s, "%x", result);
    if (result == 0) {
        s += " out=";
        hex_to(s, rsp.out.data(), rsp.out.size());
        out_handles(s, "hc", rsp.out_copy);
        out_handles(s, "hm", rsp.out_move);
        if (!rsp.out_objects.empty()) {
            s += " o=";
            for (size_t i = 0; i < rsp.out_objects.size(); ++i) {
                if (i) s += ',';
                s += rsp.out_objects[i] ? rsp.out_objects[i]->name() : std::string("-");
                if (i < reply_sids.size() && reply_sids[i] && rsp.out_objects[i]) g.sids[reply_sids[i]] = rsp.out_objects[i];
            }
        }
    }
    out_bufs(s, 'b', req.b);
    out_bufs(s, 'c', req.c);
    out_bufs(s, 'w', req.w);
    state_tail(s);
    emit(s);
}

// events seen so far: their state when one changed; shared memories whose contents changed
void state_tail(std::string& s) {
    std::string ev;
    for (const auto& e : g.event_list) ev += e->signaled ? '1' : '0';
    if (ev != g.last_ev) {
        s += " ev=" + (ev.empty() ? std::string("-") : ev);
        g.last_ev = ev;
    }
    std::string sh;
    for (size_t i = 0; i < g.shm_list.size(); ++i) {
        const auto& m = g.shm_list[i];
        const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(m->host));
        if (!p || (g.shm_copy[i].size() == m->size && std::memcmp(p, g.shm_copy[i].data(), m->size) == 0)) continue;
        g.shm_copy[i].assign(p, p + m->size);
        const uint64_t hsh = fnv(p, m->size);
        if (hsh != g.shm_hash[i]) {
            g.shm_hash[i] = hsh;
            appendf(sh, "%s%zu:%016llx", sh.empty() ? "" : ",", i, static_cast<unsigned long long>(hsh));
        }
    }
    if (!sh.empty()) s += " shm=" + sh;
}


nxrt::services::ButtonFace g_provider_face;
nxrt::services::ButtonFace provider_face() { return g_provider_face; }

void am_exit_hook(int code, const char* why) { emit("H exit " + std::to_string(code) + " " + (why ? why : "")); }

// ---- hid ----------------------------------------------------------------------------------------------------------------------
std::shared_ptr<nxrt::hid::ManualInputSource> g_hid_src;   // the scripted input source (api hid_src manual)

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void vib_sink(uint32_t slot, int hand, const nxrt::hid::VibrationValue& v, void*) {
    std::string s;
    appendf(s, "V %u %d %08x %08x %08x %08x", slot, hand, float_bits(v.amp_low), float_bits(v.freq_low), float_bits(v.amp_high),
            float_bits(v.freq_high));
    emit(s);
}

struct ReplayWebHost : nxrt::webview::Host {
    bool open(const nxrt::webview::Page&, nxrt::webview::Events) override { return false; }
    void show() override {}
    void send(const std::string&) override {}
    void close() override {}
    void pad(uint64_t buttons, int32_t lx, int32_t ly) override {
        std::string s;
        appendf(s, "W pad %llx %d %d", static_cast<unsigned long long>(buttons), lx, ly);
        emit(s);
    }
};
ReplayWebHost g_web_host;

// What one input read gave: every connected slot, the touch count, the keyboard, the mouse and the buttons hid published.
void snapshot_text(std::string& s, const nxrt::hid::InputSnapshot& in) {
    for (uint32_t i = 0; i < nxrt::hid::kNpadSlots; ++i) {
        const nxrt::hid::NpadInput& p = in.npad[i];
        if (!p.connected) continue;
        appendf(s, " p%u:%u:%llx:%d,%d,%d,%d:%d", i, static_cast<unsigned>(p.style), static_cast<unsigned long long>(p.buttons), p.left.x,
                p.left.y, p.right.x, p.right.y, p.six_axis.at_rest ? 1 : 0);
    }
    appendf(s, " t=%u k=%d:%x:%llx,%llx,%llx,%llx m=%d:%d,%d,%d,%d,%d,%d:%x sb=%llx", in.touch.count, in.keyboard_connected ? 1 : 0,
            in.keyboard.modifiers, static_cast<unsigned long long>(in.keyboard.keys[0]), static_cast<unsigned long long>(in.keyboard.keys[1]),
            static_cast<unsigned long long>(in.keyboard.keys[2]), static_cast<unsigned long long>(in.keyboard.keys[3]),
            in.mouse.connected ? 1 : 0, in.mouse.x, in.mouse.y, in.mouse.dx, in.mouse.dy, in.mouse.wheel_dx, in.mouse.wheel_dy,
            in.mouse.buttons, static_cast<unsigned long long>(nxrt::hid::sampled_buttons()));
}

// The api lines that change the scripted source: a copy of its snapshot, changed, put back. False for an unknown line.
bool hid_source_line(const std::string& what, std::istringstream& is) {
    if (!g_hid_src) return what == "hid_pad" || what == "hid_six" || what == "hid_color" || what == "hid_touch" || what == "hid_kbd" ||
                           what == "hid_mouse";
    nxrt::hid::InputSnapshot snap = g_hid_src->get();
    auto hex = [&]() -> uint64_t {
        std::string t;
        is >> t;
        return std::strtoull(t.c_str(), nullptr, 16);
    };
    auto num = [&]() -> int64_t {
        std::string t;
        is >> t;
        return std::strtoll(t.c_str(), nullptr, 10);
    };
    auto flt = [&]() -> float {
        std::string t;
        is >> t;
        return std::strtof(t.c_str(), nullptr);
    };
    if (what == "hid_pad" || what == "hid_six" || what == "hid_color") {
        const int64_t slot = num();
        if (slot < 0 || slot >= static_cast<int64_t>(nxrt::hid::kNpadSlots)) return true;
        nxrt::hid::NpadInput& p = snap.npad[slot];
        if (what == "hid_pad") {
            p.connected = num() != 0;
            p.style = static_cast<nxrt::hid::NpadStyle>(num());
            p.buttons = hex();
            p.left.x = static_cast<int32_t>(num());
            p.left.y = static_cast<int32_t>(num());
            p.right.x = static_cast<int32_t>(num());
            p.right.y = static_cast<int32_t>(num());
        } else if (what == "hid_six") {
            p.six_axis.at_rest = num() != 0;
            for (float& f : p.six_axis.accel) f = flt();
            for (float& f : p.six_axis.gyro) f = flt();
            for (float& f : p.six_axis.angle) f = flt();
            if (is >> std::ws && !is.eof())
                for (float& f : p.six_axis.direction) f = flt();
        } else {
            p.color_body = static_cast<uint32_t>(hex());
            p.color_buttons = static_cast<uint32_t>(hex());
        }
    } else if (what == "hid_touch") {
        snap.touch = nxrt::hid::TouchInput{};
        snap.touch.count = static_cast<uint32_t>(num());
        std::string f;
        for (uint32_t k = 0; k < 16 && (is >> f); ++k) {
            const std::vector<std::string> v = split(f, ':');
            if (v.size() != 6) break;
            nxrt::hid::TouchFinger& t = snap.touch.fingers[k];
            t.id = static_cast<uint32_t>(std::strtoul(v[0].c_str(), nullptr, 10));
            t.x = static_cast<uint32_t>(std::strtoul(v[1].c_str(), nullptr, 10));
            t.y = static_cast<uint32_t>(std::strtoul(v[2].c_str(), nullptr, 10));
            t.diameter_x = static_cast<uint32_t>(std::strtoul(v[3].c_str(), nullptr, 10));
            t.diameter_y = static_cast<uint32_t>(std::strtoul(v[4].c_str(), nullptr, 10));
            t.angle = static_cast<int32_t>(std::strtol(v[5].c_str(), nullptr, 10));
        }
    } else if (what == "hid_kbd") {
        snap.keyboard_connected = num() != 0;
        snap.keyboard.modifiers = static_cast<uint32_t>(hex());
        for (uint64_t& k : snap.keyboard.keys) k = hex();
    } else if (what == "hid_mouse") {
        snap.mouse.connected = num() != 0;
        snap.mouse.x = static_cast<int32_t>(num());
        snap.mouse.y = static_cast<int32_t>(num());
        snap.mouse.dx = static_cast<int32_t>(num());
        snap.mouse.dy = static_cast<int32_t>(num());
        snap.mouse.wheel_dx = static_cast<int32_t>(num());
        snap.mouse.wheel_dy = static_cast<int32_t>(num());
        snap.mouse.buttons = static_cast<uint32_t>(hex());
    } else {
        return false;
    }
    g_hid_src->set(snap);
    return true;
}

// The hid api lines; false when `what` is none of them.
bool replay_hid(const std::string& what, std::istringstream& is, std::string& s) {
    namespace sv = nxrt::services;
    namespace hid = nxrt::hid;
    if (what == "hid_bg") {
        int on = 0;
        is >> on;
        sv::hid_set_background_sampling(on != 0);
    } else if (what == "hid_sample") {
        int n = 1;
        is >> n;
        for (int i = 0; i < n && i < 100000; ++i) sv::hid_sample_once();
    } else if (what == "hid_shm") {
        const uint8_t* p = sv::hid_shared_memory_for_tests();
        if (!p) s += " none";
        else appendf(s, " %016llx", static_cast<unsigned long long>(fnv(p, sv::hidshm::kSize)));
    } else if (what == "hid_live") {
        appendf(s, " %d", sv::hid_sampler_live() ? 1 : 0);
    } else if (what == "hid_mode_changed") {
        sv::hid_mode_changed();
    } else if (what == "hid_signal") {
        uint32_t i = 0;
        is >> i;
        sv::hid_signal_style_event(i);
    } else if (what == "hid_sets") {
        uint32_t styles = 0xEEEEEEEE, mask = 0xEEEEEEEE;
        sv::hid_supported_sets(&styles, &mask);
        appendf(s, " styles=%x mask=%x players=%x", styles, mask, hid::supported_player_mask());
    } else if (what == "hid_read" || what == "hid_raw") {
        hid::InputSnapshot in;
        if (what == "hid_read") sv::hid_read_input(&in);
        else sv::hid_read_raw_input(&in);
        snapshot_text(s, in);
    } else if (what == "hid_src") {
        std::string kind;
        is >> kind;
        g_hid_src = kind == "manual" ? std::make_shared<hid::ManualInputSource>() : nullptr;
        hid::set_input_source(g_hid_src);
    } else if (what == "hid_vib_sink") {
        int on = 0;
        is >> on;
        hid::set_vibration_sink(on ? &vib_sink : nullptr, nullptr);
    } else if (what == "hid_bench_open") {
        int max = 0;
        is >> max;
        sv::hid_applet_open(max);
        appendf(s, " mask=%x", sv::hid_bench_mask());
    } else if (what == "hid_bench_close") {
        int max = 0;
        is >> max;
        uint32_t sel = 0xEEEEEEEE;
        const int n = sv::hid_applet_close(max, &sel);
        appendf(s, " players=%d sel=%x mask=%x", n, sel, sv::hid_bench_mask());
    } else if (what == "hid_bench") {
        appendf(s, " mask=%x", sv::hid_bench_mask());
    } else if (what == "web_hold") {
        int on = 0;
        is >> on;
        nxrt::webview::set_host(on ? &g_web_host : nullptr);
        nxrt::webview::set_input_held(on != 0);
    } else if (what == "jc2_mode") {
        std::string m;
        is >> m;
        hid::jc2_mouse_set_mode(m == "off" ? hid::Jc2MouseMode::Off : m == "on" ? hid::Jc2MouseMode::On : hid::Jc2MouseMode::Auto);
    } else if (what == "jc2_feed") {
        hid::HostMouse m;
        std::string t[7];
        for (std::string& x : t) is >> x;
        m.u = std::strtof(t[0].c_str(), nullptr);
        m.v = std::strtof(t[1].c_str(), nullptr);
        m.buttons = static_cast<uint32_t>(std::strtoul(t[2].c_str(), nullptr, 16));
        m.wheel_x = std::strtof(t[3].c_str(), nullptr);
        m.wheel_y = std::strtof(t[4].c_str(), nullptr);
        m.focused = t[5] == "1";
        m.captured = t[6] == "1";
        hid::jc2_mouse_feed(m);
    } else if (what == "jc2_request") {
        int on = 0;
        is >> on;
        hid::jc2_mouse_request(on != 0);
    } else if (what == "jc2_space") {
        int w = 0, h = 0;
        is >> w >> h;
        hid::jc2_mouse_set_pointer_space(w, h);
    } else if (what == "jc2_state") {
        appendf(s, " mode=%s wants=%d active=%d listed=%d", hid::jc2_mouse_mode_name(hid::jc2_mouse_mode()), hid::jc2_mouse_wants_pointer() ? 1 : 0,
                hid::jc2_mouse_active() ? 1 : 0, hid::jc2_mouse_listed() ? 1 : 0);
    } else if (hid_source_line(what, is)) {
        if (!g_hid_src) s += " no-source";
    } else {
        return false;
    }
    return true;
}

// A pointer argument of `api hle`: kept alive in `keep` until the call returned.
uint64_t hle_arg(const std::string& a, std::vector<std::vector<uint8_t>>& keep) {
    if (a == "null") return 0;
    std::vector<uint8_t> v;
    if (a.rfind("u32:", 0) == 0) {
        const uint32_t x = static_cast<uint32_t>(std::strtoul(a.c_str() + 4, nullptr, 16));
        v.resize(4);
        std::memcpy(v.data(), &x, 4);
    } else if (a.rfind("bytes:", 0) == 0) {
        v = unhex(a.substr(6));
    } else {
        return std::strtoull(a.c_str(), nullptr, 16);
    }
    v.resize(v.size() + 16, 0xEE);   // a read past the argument sees 0xEE
    keep.push_back(std::move(v));
    return reinterpret_cast<uint64_t>(keep.back().data());
}

void replay_api(const std::string& line) {
    std::istringstream is(line.substr(4));
    std::string what;
    is >> what;
    std::string s = "A " + what;
    if (what == "hle") {
        std::string name, a0, a1;
        is >> name >> a0 >> a1;
        std::vector<std::vector<uint8_t>> keep;
        keep.reserve(4);
        static ::Ctx c;
        std::memset(&c, 0, sizeof c);
        c.x[0] = hle_arg(a0, keep);
        c.x[1] = hle_arg(a1, keep);
        const uint64_t in1 = c.x[1];
        const nxrt::HleFn fn = nxrt::hle_lookup(name.c_str());
        if (!fn) {
            s += " " + name + " none";
        } else {
            fn(&c);
            appendf(s, " %s x0=%llx x1%s", name.c_str(), static_cast<unsigned long long>(c.x[0]), c.x[1] == in1 ? "=same" : "=changed");
        }
    } else if (what == "lang_hooks") {
        nxrt::services::install_language_compare_hooks();
    } else if (what == "accuid") {
        uint64_t hi = 0, lo = 0;
        nxrt::services::accapi::user_uid(&hi, &lo);
        appendf(s, " %016llx %016llx", static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
        nxrt::services::accapi::user_uid(nullptr, nullptr);
    } else if (what == "face_set" || what == "face_provider") {
        nxrt::services::ButtonFace f;
        std::string style = "xbox", names;
        if (what == "face_set") is >> style >> names;
        else names = "xbox/b,xbox/a,xbox/y,xbox/x,xbox/lb,xbox/rb,xbox/lt,xbox/rt,-,-,xbox/menu,xbox/view,-,-,-,-,-,-";
        f.style = style;
        const std::vector<std::string> v = split(names, ',');
        for (size_t i = 0; i < f.name.size() && i < v.size(); ++i) f.name[i] = v[i] == "-" ? std::string() : v[i];
        if (what == "face_set") {
            nxrt::services::shared_font_set_face(f);
        } else {
            g_provider_face = f;
            nxrt::services::shared_font_set_face_provider(&provider_face);
        }
    } else if (what == "face_get") {
        const nxrt::services::ButtonFace f = nxrt::services::shared_font_face();
        s += " " + f.style;
        for (const std::string& n : f.name) s += " " + (n.empty() ? std::string("-") : n);
    } else if (what == "vi_present") {
        nxrt::services::victl::note_present();
    } else if (what == "vi_frames") {
        appendf(s, " %llu", static_cast<unsigned long long>(nxrt::services::victl::frames_presented()));
    } else if (what == "vi_history") {
        // the entries' status and frame number, and how their times relate (the times themselves vary run to run)
        int max = 0;
        is >> max;
        uint64_t e[5 * 12];
        std::memset(e, 0xEE, sizeof e);
        const int n = nxrt::services::victl::frame_history(max > 0 ? e : nullptr, max > 12 ? 12 : max);
        appendf(s, " n=%d", n);
        for (int i = 0; i < n; ++i)
            appendf(s, " %llu:%llu:%s%s", static_cast<unsigned long long>(e[5 * i]), static_cast<unsigned long long>(e[5 * i + 1]),
                    e[5 * i + 2] <= e[5 * i + 3] ? "q<=p" : "q>p", e[5 * i + 3] == e[5 * i + 4] ? "" : ":p!=p");
        if (n >= 0 && n < 12) appendf(s, " next=%s", e[5 * n] == 0xEEEEEEEEEEEEEEEEull ? "untouched" : "written");
    } else if (what == "vi_vsync") {
        appendf(s, " %s", nxrt::services::viapi::display_vsync_handle() ? "handle" : "none");
    } else if (what == "am_docked" || what == "am_focus" || what == "am_applet") {
        int on = 0;
        is >> on;
        if (what == "am_docked") nxrt::services::amctl::set_docked(on != 0);
        else if (what == "am_focus") nxrt::services::amctl::set_focus(on != 0);
        else nxrt::services::amctl::applet_open(on != 0);
    } else if (what == "am_state") {
        appendf(s, " docked=%d in_focus=%d", nxrt::services::amctl::docked() ? 1 : 0, nxrt::services::amctl::in_focus() ? 1 : 0);
    } else if (what == "am_exit_hook") {
        nxrt::services::amctl::set_exit_hook(&am_exit_hook);
    } else if (what == "file" || what == "file_rm") {
        std::string path;
        std::getline(is >> std::ws, path);
        if (what == "file_rm") {
            std::remove(path.c_str());
        } else {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            const std::streamoff n = f ? static_cast<std::streamoff>(f.tellg()) : -1;
            if (n < 0) {
                s += " none";
            } else {
                std::vector<uint8_t> v(static_cast<size_t>(n));
                f.seekg(0);
                if (n) f.read(reinterpret_cast<char*>(v.data()), n);
                appendf(s, " %llu ", static_cast<unsigned long long>(n));
                if (n <= 4096) hex_to(s, v.data(), v.size());
                else appendf(s, "#%016llx", static_cast<unsigned long long>(fnv(v.data(), v.size())));
            }
        }
    } else if (what == "sleep_ms") {
        int ms = 0;
        is >> ms;
        if (ms > 0 && ms <= 10000) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    } else if (what == "tmem") {
        for (size_t i = 0; i < g.tmems.size(); ++i) {
            const auto& t = g.tmems[i];
            const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(t->addr));
            appendf(s, " t%zu:%llx:%016llx", i, static_cast<unsigned long long>(t->size),
                    static_cast<unsigned long long>(p && t->size ? fnv(p, t->size) : 0));
        }
    } else if (!replay_hid(what, is, s)) {
        s += " unknown";
    }
    state_tail(s);
    emit(s);
}

}  // namespace

int main(int argc, char** argv) {
    std::string script, out, tmpdir;
    int timeout_s = 20;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--script" && i + 1 < argc) script = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--tmp" && i + 1 < argc) tmpdir = argv[++i];
        else if (a == "--timeout" && i + 1 < argc) timeout_s = std::atoi(argv[++i]);
    }
    if (script.empty() || out.empty()) {
        std::fprintf(stderr, "usage: nxrt_ipc_replay --script <file> --out <file> [--tmp <folder>] [--timeout <s>]\n");
        return 2;
    }
    std::ifstream in(script, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", script.c_str());
        return 2;
    }
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
    }
    // the objects each recorded reply returned (A ... o=<sid>:<iface>,...)
    std::unordered_map<uint64_t, std::vector<uint32_t>> reply_sids;
    for (const std::string& l : lines) {
        if (l.rfind("A ", 0) != 0) continue;
        std::istringstream is(l);
        std::string tag, tok;
        uint64_t seq = 0;
        is >> tag >> seq;
        while (is >> tok)
            if (tok.rfind("o=", 0) == 0)
                for (const std::string& e : split(tok.substr(2), ','))
                    reply_sids[seq].push_back(static_cast<uint32_t>(std::strtoul(e.c_str(), nullptr, 10)));
    }

    g_out = std::fopen(out.c_str(), "wb");
    if (!g_out) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 2;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp = tmpdir.empty() ? fs::temp_directory_path(ec) / "nxrt_ipc_replay" : fs::path(tmpdir);
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "save", ec);
    fs::create_directories(tmp / "album", ec);
    fs::create_directories(tmp / "romfs", ec);
    fs::create_directories(tmp / "aoc", ec);
    const std::string log_path = (tmp / "nxrt_ipc_replay.log").string();
    nxrt::LogOptions lo;
    lo.path = log_path.c_str();
    lo.level = nxrt::LogLevel::Info;
    lo.to_stderr = false;
    lo.keep_previous = false;
    nxrt::log_init(lo);
    nxrt::config_set("fs", "save_dir", (tmp / "save").string().c_str());
    nxrt::config_set("fs", "romfs", (tmp / "romfs").string().c_str());
    nxrt::config_set("album", "dir", (tmp / "album").string().c_str());
    nxrt::config_set("fs", "aoc_dir", (tmp / "aoc").string().c_str());
    nxrt::config_set("audio", "sink", "null");
    nxrt::config_set("net", "protocol", "linkplay");
    nxrt::config_set("net", "lan_discovery", "off");
    nxrt::mem::init();
    nxrt::services::register_all();
    nxrt::services::reset_all();

    // a call that does not return: the line says so and the replay ends (the state after it is not comparable)
    std::thread([timeout_s] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const int64_t t0 = g_call_started_ms.load();
            if (t0 >= 0 && now_ms() - t0 > timeout_s * 1000LL) {
                std::fprintf(g_out, "TIMEOUT %llu\n", static_cast<unsigned long long>(g_running_seq.load()));
                std::fflush(g_out);
                std::_Exit(3);
            }
        }
    }).detach();

    for (const std::string& l : lines) {
        if (l.empty() || l[0] == '#') continue;
        if (l.rfind("Q ", 0) == 0) {
            Q q;
            if (!parse_q(l, q)) {
                emit("BADLINE " + l.substr(0, 80));
                continue;
            }
            const auto r = reply_sids.find(q.seq);
            replay_q(q, !q.as.empty() ? q.as : r != reply_sids.end() ? r->second : std::vector<uint32_t>{});
        } else if (l.rfind("X ", 0) == 0) {
            const uint32_t sid = static_cast<uint32_t>(std::strtoul(l.c_str() + 2, nullptr, 10));
            auto it = g.sids.find(sid);
            if (it != g.sids.end()) {
                std::shared_ptr<ipc::Service> s = std::move(it->second);
                g.sids.erase(it);
                if (s.use_count() == 1) s->on_close();
                emit("X " + std::to_string(sid));
            }
        } else if (l.rfind("api ", 0) == 0) {
            replay_api(l);
        } else if (l == "reset" || l == "reset keep") {
            if (l == "reset") g.sids.clear();
            nxrt::services::reset_all();
            emit(l);
        } else if (l.rfind("config ", 0) == 0) {
            std::istringstream is(l.substr(7));
            std::string sec, key, val;
            is >> sec >> key;
            std::getline(is, val);
            if (!val.empty() && val[0] == ' ') val.erase(0, 1);
            nxrt::config_set(sec.c_str(), key.c_str(), val.c_str());
            nxrt::services::profile_reload();
            emit("config " + sec + " " + key);
        }
    }
    std::fputs("END\n", g_out);
    std::fflush(g_out);
    nxrt::log_flush();
    std::_Exit(0);
}
