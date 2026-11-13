// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// nxrt_services_test: the service tests (svc_test.h). PokoMac carries AnySwitch's harness without its command-table probe (--table,
// --layouts) and its multi-program and profiler children.
//
//   nxrt_services_test [family ...]           run every case (or the named families); exit 1 when a check fails
//   nxrt_services_test --list                 the families and cases
//
// No game is loaded: the services answer from their defaults (services::profile()), the save and album folders are a temporary folder.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "nxrt/config.h"
#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "svc_test.h"

namespace svctest {

std::vector<Case>& cases() {
    static std::vector<Case> v;
    return v;
}

int g_checks = 0;
static int g_failures = 0;
static const Case* g_current = nullptr;

void fail(const char* file, int line, const std::string& what) {
    ++g_failures;
    const char* f = file;
    for (const char* p = file; *p; ++p)
        if (*p == '/' || *p == '\\') f = p + 1;
    std::fprintf(stderr, "FAIL %s.%s (%s:%d): %s\n", g_current ? g_current->family : "?", g_current ? g_current->name : "?", f, line,
                 what.c_str());
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
}

namespace {

constexpr uint32_t kSfci = 0x49434653, kSfco = 0x4F434653;
constexpr size_t kMsgWords = 0x100 / 4;   // the TLS message buffer the SDK uses

void put32(std::vector<uint8_t>& v, uint32_t x) {
    const size_t o = v.size();
    v.resize(o + 4);
    std::memcpy(v.data() + o, &x, 4);
}

uint32_t rd32(const std::vector<uint8_t>& v, size_t o) {
    uint32_t x = 0;
    if (o + 4 <= v.size()) std::memcpy(&x, v.data() + o, 4);
    return x;
}

// Sends `m` on `handle` and parses the reply; `domain` replies start with the domain header.
Reply send(uint32_t handle, hipc::Message& m, bool domain) {
    Reply out;
    alignas(16) uint32_t words[kMsgWords] = {};
    std::string err;
    if (!hipc::build(m, words, kMsgWords, &err)) {
        fail(__FILE__, __LINE__, "request does not build: " + err);
        return out;
    }
    out.svc_result = nxrt::ipc::send_sync_request(nullptr, handle, reinterpret_cast<uint64_t>(words), sizeof words);
    if (out.svc_result != 0) return out;
    hipc::Message rm;
    if (!hipc::parse(words, kMsgWords, rm, &err)) {
        fail(__FILE__, __LINE__, "reply does not parse: " + err);
        return out;
    }
    // raw = payload + pad_after (16 - pad_before), so the payload in whole words is raw - (16 - pad_before).
    const size_t pad_after = 16 - rm.pad_before;
    size_t payload = rm.raw.size() >= pad_after ? rm.raw.size() - pad_after : 0;
    size_t pos = 0;
    uint32_t n_obj = 0;
    if (domain) {
        n_obj = rd32(rm.raw, 0);
        pos = 16;
    }
    if (rd32(rm.raw, pos) != kSfco) {
        fail(__FILE__, __LINE__, "reply without SFCO");
        return out;
    }
    out.result = rd32(rm.raw, pos + 8);
    pos += 16;
    size_t data_end = payload;
    if (domain && n_obj) {
        // the object ids follow the data; the data is whatever lies between
        data_end = payload >= 4 * n_obj ? payload - 4 * n_obj : pos;
        for (uint32_t i = 0; i < n_obj; ++i) out.objects.push_back(rd32(rm.raw, data_end + 4 * i));
    }
    if (data_end > pos) out.data.assign(rm.raw.begin() + pos, rm.raw.begin() + data_end);
    out.copy = rm.copy;
    for (uint32_t h : rm.move) {
        if (!domain && nxrt::kernel::handle_as<nxrt::ipc::KSession>(h))
            out.objects.push_back(h);
        else
            out.move.push_back(h);
    }
    out.ok = true;
    return out;
}

// Every buffer goes through guest memory (the kernel region, below 2^39 as the descriptors require, like a console address): the
// request's contents are copied in, and after the reply the B / C buffers are copied back to the caller's memory.
struct Bounce {
    uint8_t* guest;
    const Buf* user;
};
// One pool per thread: every call bounces its buffers from the pool's start, so two threads calling at once (a case with a sender and a
// receiver thread, test_ldn_peer.cpp's TCP echo) would overwrite each other's buffers in a shared one.
uint8_t* bounce_pool(size_t* cap) {
    static thread_local uint8_t* pool = reinterpret_cast<uint8_t*>(nxrt::mem::alloc_kernel(32u << 20));
    *cap = 32u << 20;
    return pool;
}

std::vector<Bounce> add_buffers(hipc::Message& m, const Req& r) {
    std::vector<Bounce> out;
    size_t cap = 0, used = 0;
    uint8_t* pool = bounce_pool(&cap);
    uint32_t xi = 0;
    for (const Buf& b : r.bufs) {
        uint8_t* g = nullptr;
        if (b.p && b.size && pool && used + b.size <= cap) {
            g = pool + used;
            used = (used + b.size + 15) & ~size_t{15};
            std::memcpy(g, b.p, b.size);
            out.push_back({g, &b});
        }
        const uint64_t a = reinterpret_cast<uint64_t>(g);
        switch (b.kind) {
            case 'A': m.send.push_back({a, b.size, 0}); break;
            case 'B': m.recv.push_back({a, b.size, 0}); break;
            case 'X': m.ptrs.push_back({a, static_cast<uint32_t>(b.size), xi++}); break;
            case 'C': m.recv_list.push_back({a, static_cast<uint32_t>(b.size)}); break;
        }
    }
    if (!m.recv_list.empty()) m.recv_list_flags = m.recv_list.size() == 1 ? 2u : static_cast<uint32_t>(m.recv_list.size() + 2);
    return out;
}

void copy_back(const std::vector<Bounce>& v) {
    for (const Bounce& b : v)
        if (b.user->kind == 'B' || b.user->kind == 'C') std::memcpy(b.user->p, b.guest, b.user->size);
}

Session g_sm;

}  // namespace

Reply Session::call(uint32_t cmd, const Req& r) const {
    hipc::Message m;
    m.type = hipc::kInvoke2MethodOld;   // 4: Request (the SDK's CMIF request type)
    m.has_pid = r.pid;
    m.pid = 0x51;
    m.copy = r.copy;
    const std::vector<Bounce> bounced = add_buffers(m, r);
    std::vector<uint8_t> body;
    put32(body, kSfci);
    put32(body, 0);
    put32(body, cmd);
    put32(body, 0);
    body.insert(body.end(), r.raw.begin(), r.raw.end());
    while (body.size() % 4) body.push_back(0);
    if (domain_) {
        // CmifDomainInHeader: type 1 (send message), object count, data size, object id, padding, token; the object ids follow the data.
        m.raw.push_back(1);
        m.raw.push_back(static_cast<uint8_t>(r.objs.size()));
        const uint16_t sz = static_cast<uint16_t>(body.size());
        m.raw.push_back(static_cast<uint8_t>(sz));
        m.raw.push_back(static_cast<uint8_t>(sz >> 8));
        put32(m.raw, object_);
        put32(m.raw, 0);
        put32(m.raw, 0);
        m.raw.insert(m.raw.end(), body.begin(), body.end());
        for (uint32_t o : r.objs) put32(m.raw, o);
    } else {
        m.move = r.objs;
        m.raw = body;
    }
    Reply rp = send(handle_, m, domain_);
    copy_back(bounced);
    return rp;
}

Session Session::object(const Reply& rp, size_t i) const {
    if (i >= rp.objects.size()) return Session();
    if (domain_) return Session(handle_, true, rp.objects[i]);
    return Session(rp.objects[i]);
}

bool Session::to_domain() {
    hipc::Message m;
    m.type = hipc::kInvoke2ManagerMethodOld;   // 5: Control
    put32(m.raw, kSfci);
    put32(m.raw, 0);
    put32(m.raw, 0);   // ConvertCurrentObjectToDomain
    put32(m.raw, 0);
    Reply r = send(handle_, m, false);
    if (!r.ok || r.result != 0 || r.words() != 1) return false;
    domain_ = true;
    object_ = r.get<uint32_t>(0);
    return true;
}

void Session::close() {
    if (!handle_) return;
    if (domain_ && object_ != 1) {
        hipc::Message m;
        m.type = hipc::kInvoke2MethodOld;
        m.raw.push_back(2);   // close the object
        m.raw.push_back(0);
        m.raw.push_back(0);
        m.raw.push_back(0);
        put32(m.raw, object_);
        put32(m.raw, 0);
        put32(m.raw, 0);
        alignas(16) uint32_t words[kMsgWords] = {};
        if (hipc::build(m, words, kMsgWords, nullptr)) nxrt::ipc::send_sync_request(nullptr, handle_, reinterpret_cast<uint64_t>(words), sizeof words);
    } else {
        nxrt::kernel::handle_close(handle_);
    }
    handle_ = 0;
}

Session open(const char* name) {
    if (!g_sm.valid()) {
        uint32_t h = 0;
        if (nxrt::ipc::connect_named_port("sm:", &h) != 0) return Session();
        g_sm = Session(h);
        g_sm.call(0, Req().with_pid());   // RegisterClient
    }
    uint64_t n = 0;
    std::memcpy(&n, name, std::min<size_t>(std::strlen(name), 8));
    Reply r = g_sm.call(1, Req().put(n));
    if (!r.ok || r.result != 0 || r.objects.size() != 1) return Session();
    return Session(r.objects[0]);
}

void reset() { nxrt::services::reset_all(); }

}  // namespace svctest

using namespace svctest;

int main(int argc, char** argv) {
    std::set<std::string> only;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") list = true;
        else if (a == "--log" && i + 1 < argc) ++i;                              // what the launcher passes a game: not used here
        else only.insert(a);
    }
    std::sort(cases().begin(), cases().end(), [](const Case& a, const Case& b) {
        const int c = std::strcmp(a.family, b.family);
        return c != 0 ? c < 0 : std::strcmp(a.name, b.name) < 0;
    });
    if (list) {
        for (const Case& c : cases()) std::printf("%s %s\n", c.family, c.name);
        return 0;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / "nxrt_services_test";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "save", ec);
    fs::create_directories(tmp / "album", ec);
    fs::create_directories(tmp / "romfs" / "dir", ec);
    if (FILE* f = std::fopen((tmp / "romfs" / "probe.txt").string().c_str(), "wb")) {   // what the probe opens as a file
        std::fputs("probe", f);
        std::fclose(f);
    }
    // Two add-ons of this (program id 0) title: base id 0x1000, so 0x1001 and 0x1002 (content tests: aoc listing and mounting).
    fs::create_directories(tmp / "aoc" / "0000000000001001" / "romfs", ec);
    fs::create_directories(tmp / "aoc" / "0000000000001002" / "romfs", ec);
    if (FILE* f = std::fopen((tmp / "aoc" / "0000000000001001" / "romfs" / "hello.txt").string().c_str(), "wb")) {
        std::fputs("add-on one", f);
        std::fclose(f);
    }
    const std::string log_path = (tmp / "nxrt_services_test.log").string();
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
    nxrt::config_set("audio", "sink", "null");   // no host audio device is opened by the tests
    nxrt::config_set("net", "protocol", "linkplay");
    nxrt::config_set("net", "lan_discovery", "off");
    nxrt::mem::init();

    nxrt::services::register_all();
    nxrt::services::reset_all();

    int ran = 0;
    for (const Case& c : cases()) {
        if (!only.empty() && !only.count(c.family)) continue;
        g_current = &c;
        const int before = g_failures;
        reset();
        c.fn();
        ++ran;
        std::printf("%-6s %s.%s\n", g_failures == before ? "ok" : "FAILED", c.family, c.name);
    }
    g_current = nullptr;

    std::printf("%d cases, %d checks, %d failures (log: %s)\n", ran, g_checks, g_failures, log_path.c_str());
    nxrt::log_flush();
    std::fflush(stdout);
    std::_Exit(g_failures ? 1 : 0);   // no static destructors: service threads (sockets, ldn) may still hold state
}
