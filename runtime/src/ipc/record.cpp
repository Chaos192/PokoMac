// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The request recorder ([ipc] record, include/nxrt/ipc.h): every request a service handles, its reply and every service closed, one
// line each, so a run of a game can be replayed against two builds of a service (tools/services/ipc_diff, runtime/tests/ipc_replay).
//
// File format (text, one event per line, fields separated by one space; <hex> is lower-case bytes, "-" when empty):
//   H nxrt-ipc-record 1 program=<16 hex digits>
//   Q <seq> <thread> <sid> <iface> <cmd> <msg type> <pid> in=<hex> [a<n>=<size>:<hex>] [x<n>=..] [w<n>=..] [b<n>=<size>] [c<n>=<size>]
//     [hc=<handle,...>] [hm=<handle,...>] [os=<sid,...>]
//   A <seq> <result> out=<hex> [hc=<handle,...>] [hm=<handle,...>] [o=<sid>:<iface>,...] [b<n>=<contents>] [c<n>=..] [w<n>=..]
//   X <sid>                      the service was closed (its last session or domain object went away: Service::on_close)
//   D <iface> <cmd>              this command reached the per-command cap: its further calls are not written
// [ipc] record, or [test] ipc_record (env NXRT_TEST_IPC_RECORD, for runs through the launcher), names the file.
// <sid> numbers the service objects in the order the recorder first meets them (a service handed out as an object, or the target of a
// request it has not seen handed out: sm: and the ports). A handle is e (event), p (process), s<size>:<hash> (shared memory and the
// FNV-1a 64 of its contents), t<size> (transfer memory), S<sid> (session), T (thread) or u (anything else). An input buffer's contents
// are written up to 64 KiB ("#" after the size: longer, contents left out); an output buffer is written in hex up to 64 bytes, else
// as #<FNV-1a 64>. Each (iface, cmd) is written for its first [ipc] record_per_cmd calls (default 400); the file stops growing at
// [ipc] record_max_mb (default 512).
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/services.h"

namespace nxrt::ipc::record {
namespace {

constexpr uint64_t kInCap = 64 * 1024;   // input buffer contents written up to this size
constexpr uint64_t kOutHex = 64;         // output buffers up to this size in hex, larger ones as a hash

std::atomic<int> g_state{-1};            // -1 not decided yet, 0 off, 1 on

struct Recorder {
    std::mutex mu;
    FILE* f = nullptr;
    uint64_t seq = 0;
    uint64_t written = 0, max_bytes = 0;
    uint32_t per_cmd = 0;
    uint32_t next_sid = 1;
    std::unordered_map<const Service*, uint32_t> sids;
    std::map<std::pair<std::string, uint32_t>, uint32_t> counts;
};

Recorder& rec() {
    static Recorder r;
    return r;
}

uint64_t fnv(const uint8_t* p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void hex_to(std::string& s, const uint8_t* p, uint64_t n) {
    static const char* d = "0123456789abcdef";
    if (!p || !n) {
        s += '-';
        return;
    }
    s.reserve(s.size() + 2 * n);
    for (uint64_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
}

void appendf(std::string& s, const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    s += b;
}

const uint8_t* host(uint64_t addr) { return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr)); }

uint32_t sid_of(Recorder& r, const Service* s) {   // caller holds r.mu
    if (!s) return 0;
    auto it = r.sids.find(s);
    if (it != r.sids.end()) return it->second;
    return r.sids[s] = r.next_sid++;
}

void handle_to(Recorder& r, std::string& s, uint32_t h) {
    auto o = kernel::handle_get(h);
    if (!o) {
        s += 'u';
        return;
    }
    switch (o->type()) {
        case kernel::ObjType::Event: s += 'e'; break;
        case kernel::ObjType::Process: s += 'p'; break;
        case kernel::ObjType::Thread: s += 'T'; break;
        case kernel::ObjType::SharedMemory: {
            auto m = std::static_pointer_cast<kernel::KSharedMemory>(o);
            appendf(s, "s%llx:%016llx", static_cast<unsigned long long>(m->size),
                    static_cast<unsigned long long>(m->host ? fnv(host(m->host), m->size) : 0));
            break;
        }
        case kernel::ObjType::TransferMemory:
            appendf(s, "t%llx", static_cast<unsigned long long>(std::static_pointer_cast<kernel::KTransferMemory>(o)->size));
            break;
        case kernel::ObjType::Session: {
            auto k = std::static_pointer_cast<KSession>(o);
            appendf(s, "S%u", sid_of(r, k->svc.get()));
            break;
        }
        default: s += 'u'; break;
    }
}

void handles_to(Recorder& r, std::string& s, const char* key, const std::vector<uint32_t>& v) {
    if (v.empty()) return;
    s += ' ';
    s += key;
    s += '=';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        handle_to(r, s, v[i]);
    }
}

void in_bufs(std::string& s, char kind, const std::vector<Buffer>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        appendf(s, " %c%zu=%llx", kind, i, static_cast<unsigned long long>(v[i].size));
        if (v[i].size > kInCap) {
            s += ":#";
        } else {
            s += ':';
            hex_to(s, v[i].addr ? host(v[i].addr) : nullptr, v[i].addr ? v[i].size : 0);
        }
    }
}

void out_bufs(std::string& s, char kind, const std::vector<Buffer>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        appendf(s, " %c%zu=", kind, i);
        if (!v[i].addr || !v[i].size) {
            s += '-';
        } else if (v[i].size <= kOutHex) {
            hex_to(s, host(v[i].addr), v[i].size);
        } else {
            appendf(s, "#%016llx", static_cast<unsigned long long>(fnv(host(v[i].addr), v[i].size)));
        }
    }
}

void write_line(Recorder& r, std::string& s) {   // caller holds r.mu
    if (!r.f) return;
    s += '\n';
    if (r.max_bytes && r.written + s.size() > r.max_bytes) {
        std::fputs("# record_max_mb reached: the recording ends here\n", r.f);
        std::fclose(r.f);
        r.f = nullptr;
        g_state.store(0);
        return;
    }
    std::fwrite(s.data(), 1, s.size(), r.f);
    std::fflush(r.f);
    r.written += s.size();
}

bool open_file(Recorder& r, const std::string& path) {   // caller holds r.mu
    if (r.f) std::fclose(r.f);
    r.f = std::fopen(path.c_str(), "wb");
    r.seq = r.written = 0;
    r.next_sid = 1;
    r.sids.clear();
    r.counts.clear();
    if (!r.f) {
        NXLOG_E("ipc", "[ipc] record: cannot write %s", path.c_str());
        return false;
    }
    r.max_bytes = static_cast<uint64_t>(config_get_int("ipc", "record_max_mb", 512)) << 20;
    r.per_cmd = static_cast<uint32_t>(config_get_int("ipc", "record_per_cmd", 400));
    std::string h;
    appendf(h, "H nxrt-ipc-record 1 program=%016llx", static_cast<unsigned long long>(services::program_id_now()));
    write_line(r, h);
    NXLOG_I("ipc", "recording every request into %s", path.c_str());
    return true;
}

bool decide() {
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    int st = g_state.load();
    if (st >= 0) return st == 1;
    // [test] ipc_record (NXRT_TEST_IPC_RECORD) is the same switch under a test name: a test run through the launcher may set NXRT_TEST_*
    // in the launcher's environment, never other NXRT_*, and the game's own ini is the player's
    std::string path = config_get_str("ipc", "record", "");   // PokoMac: relative to the game folder (the working directory)
    if (path.empty()) path = config_get_str("test", "ipc_record", "");
    const bool ok = !path.empty() && open_file(r, path);
    g_state.store(ok ? 1 : 0);
    return ok;
}

}  // namespace

bool begin(const std::string& path) {
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    const bool ok = open_file(r, path);
    g_state.store(ok ? 1 : 0);
    return ok;
}

void end() {
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.f) std::fclose(r.f);
    r.f = nullptr;
    g_state.store(0);
}

bool on() {
    const int st = g_state.load(std::memory_order_relaxed);
    return st == 1 || (st < 0 && decide());
}

uint64_t request(const Service& svc, const Request& req) {
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (!r.f) return 0;
    uint32_t& n = r.counts[{svc.name(), req.cmd}];
    if (r.per_cmd && n >= r.per_cmd) {
        if (n == r.per_cmd) {
            std::string d;
            appendf(d, "D %s %u", svc.name().c_str(), req.cmd);
            write_line(r, d);
            ++n;
        }
        return 0;
    }
    ++n;
    const uint64_t seq = ++r.seq;
    std::string s;
    const kernel::KThread* t = kernel::current_thread();
    appendf(s, "Q %llu %u %u %s %u %u %llx in=", static_cast<unsigned long long>(seq), t ? t->host_tid : 0u, sid_of(r, &svc),
            svc.name().c_str(), req.cmd, req.msg_type, static_cast<unsigned long long>(req.pid));
    hex_to(s, req.in, req.in_size);
    in_bufs(s, 'a', req.a);
    in_bufs(s, 'x', req.x);
    in_bufs(s, 'w', req.w);
    for (size_t i = 0; i < req.b.size(); ++i) appendf(s, " b%zu=%llx", i, static_cast<unsigned long long>(req.b[i].size));
    for (size_t i = 0; i < req.c.size(); ++i) appendf(s, " c%zu=%llx", i, static_cast<unsigned long long>(req.c[i].size));
    handles_to(r, s, "hc", req.in_copy);
    handles_to(r, s, "hm", req.in_move);
    if (!req.in_services.empty()) {
        s += " os=";
        for (size_t i = 0; i < req.in_services.size(); ++i) {
            if (i) s += ',';
            appendf(s, "%u", sid_of(r, req.in_services[i].get()));
        }
    }
    write_line(r, s);
    return seq;
}

void response(uint64_t seq, const Request& req, const Response& rsp, uint32_t result) {
    if (!seq) return;
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (!r.f) return;
    std::string s;
    appendf(s, "A %llu %x out=", static_cast<unsigned long long>(seq), result);
    hex_to(s, rsp.out.data(), rsp.out.size());
    handles_to(r, s, "hc", rsp.out_copy);
    handles_to(r, s, "hm", rsp.out_move);
    if (!rsp.out_objects.empty()) {
        s += " o=";
        for (size_t i = 0; i < rsp.out_objects.size(); ++i) {
            const Service* o = rsp.out_objects[i].get();
            if (i) s += ',';
            if (!o) {
                s += "0:-";
                continue;
            }
            r.sids[o] = r.next_sid++;   // a new object for the client, even when the service hands out the same one again
            appendf(s, "%u:%s", r.sids[o], o->name().c_str());
        }
    }
    out_bufs(s, 'b', req.b);
    out_bufs(s, 'c', req.c);
    out_bufs(s, 'w', req.w);
    write_line(r, s);
}

void closed(const Service& svc) {
    if (g_state.load(std::memory_order_relaxed) != 1) return;
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (!r.f) return;
    auto it = r.sids.find(&svc);
    if (it == r.sids.end()) return;
    std::string s;
    appendf(s, "X %u", it->second);
    r.sids.erase(it);
    write_line(r, s);
}

}  // namespace nxrt::ipc::record
