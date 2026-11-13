// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
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
//     api accuid                         accapi::user_uid
//     (PokoMac: AnySwitch's lang_hooks, face_set, face_get and face_provider are not here; PokoMac has none of those entry points)
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

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"

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
    } else if (what == "accuid") {
        uint64_t hi = 0, lo = 0;
        nxrt::services::accapi::user_uid(&hi, &lo);
        appendf(s, " %016llx %016llx", static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
        nxrt::services::accapi::user_uid(nullptr, nullptr);
    } else {
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
