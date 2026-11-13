// SPDX-License-Identifier: GPL-3.0-or-later
// HIPC/CMIF service framework: sessions, domains, the service base class, the service registry, the `sm:` service manager and
// stub services that log every command and answer with kernel::kStubService.
// Wire format: hipc.h (https://switchbrew.org/wiki/IPC_Marshalling). Structure after SuperMarioWonderRecomp runtime/src/ipc.h and
// ipc.cpp (MIT, Copyright (c) 2026 BelmanteGu); the service registry idea (swipc-generated tables as an option) from ScarletNX.
//
// A request enters through svcSendSyncRequest: the 0x100-byte message at the thread's TLS region (or the user buffer) is parsed with
// hipc::parse, routed to the session's service (or the domain object the message names) and answered by writing the response message
// back over the buffer. CMIF control messages (types 5 and 7) and Release (type 2) are handled here, never by a service.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/kernel.h"

namespace nxrt::ipc {

struct Buffer {
    uint64_t addr = 0, size = 0;
};

class Service;

// What a command handler gets. Guest pointers are host pointers, so buffers are directly readable/writable.
struct Request {
    ::Ctx* ctx = nullptr;                  // the calling guest thread's registers (may be null in tests)
    uint32_t cmd = 0;                      // CMIF method id, or the TIPC command (message type - 16)
    uint16_t msg_type = 0;                 // hipc message type of the request
    bool tipc = false;
    const uint8_t* in = nullptr;           // raw arguments: after the SFCI header (CMIF) or the whole raw payload (TIPC)
    size_t in_size = 0;
    uint64_t pid = 0;                      // process id sent with the request (0 if none)
    std::vector<uint32_t> in_copy, in_move;           // handles sent along
    std::vector<Buffer> a, b, w, x, c;                // A (send), B (receive), W (exchange), X (pointer), C (receive list) buffers
    std::vector<std::shared_ptr<Service>> in_services;    // interfaces passed in: domain objects or moved session handles, resolved (null = invalid)
    template <typename T>
    T arg(size_t off) const {
        T v{};
        if (off + sizeof(T) <= in_size) __builtin_memcpy(&v, in + off, sizeof(T));
        return v;
    }
    // "The n-th input buffer": A buffers first then X; "the n-th output buffer": B first then C. Empty Buffer{} when absent.
    Buffer in_buf(size_t n) const { return n < a.size() && a[n].addr ? a[n] : (n < x.size() ? x[n] : Buffer{}); }
    Buffer out_buf(size_t n) const { return n < b.size() && b[n].addr ? b[n] : (n < c.size() ? c[n] : Buffer{}); }
};

struct Response {
    std::vector<uint8_t> out;                              // raw return values (after SFCO / after the TIPC result word)
    std::vector<uint32_t> out_copy, out_move;              // handles returned
    std::vector<std::shared_ptr<Service>> out_objects;     // interfaces returned: new session handles (move) or domain object ids
    template <typename T>
    void push(const T& v) {
        size_t o = out.size();
        out.resize(o + sizeof(T));
        __builtin_memcpy(out.data() + o, &v, sizeof(T));
    }
    void push_object(std::shared_ptr<Service> s) { out_objects.push_back(std::move(s)); }
};

using Handler = std::function<uint32_t(Request&, Response&)>;   // returns a Result code (0 = success)

// ---- command extensions (src/ipc/extend.cpp; ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch) ------------------
// Commands added to an interface, by its Service name, from a file of their own (src/services/ext_*.cpp): Service::handle consults them
// only when the interface's own table has no handler for the command, so a handler the interface registers itself always wins. The
// handler gets the Service it was asked of. Registered at start-up (services::register_all), read-only afterwards.
class Service;
using ExtHandler = std::function<uint32_t(Service&, Request&, Response&)>;
void extend(const std::string& iface, uint32_t id, const char* cmd_name, ExtHandler h);
bool find_extension(const std::string& iface, uint32_t id, ExtHandler* out);   // copies the handler out (no lock held while it runs)
std::vector<std::pair<std::string, std::pair<uint32_t, const char*>>> extensions();   // (interface, (id, name)) of every extension
void extensions_clear();                                                               // tests

// ---- request recorder (src/ipc/record.cpp) ---------------------------------------------------------------------------------
// [ipc] record = <file> (off by default): every request a service handles, its reply and every service closed are written to the file,
// so a game's run can be replayed against two builds of a service (tools/services/ipc_diff, runtime/tests/ipc_replay). Format: record.cpp.
namespace record {
bool on();                                                   // decided at the first request (the config is loaded by then)
bool begin(const std::string& path);                         // tests: record into `path` from now on (a new file, sids from 1)
void end();                                                  // tests: stop and close the file
uint64_t request(const Service& svc, const Request& req);   // before the handler: the request's sequence number (0: not written)
void response(uint64_t seq, const Request& req, const Response& rsp, uint32_t result);
void closed(const Service& svc);                             // Service::on_close is about to run
}  // namespace record

class Service : public std::enable_shared_from_this<Service> {
public:
    explicit Service(std::string name) : name_(std::move(name)) {}
    virtual ~Service() = default;
    const std::string& name() const { return name_; }
    virtual bool is_stub() const { return false; }
    // Default: looks `cmd` up in the command table; unknown commands log a warning ("not implemented") and return kStubService.
    virtual uint32_t handle(Request& req, Response& rsp);
    virtual void on_close() {}                              // the last session/object referencing the service was released
    void add_cmd(uint32_t id, const char* cmd_name, Handler h) { cmds_[id] = {cmd_name, std::move(h)}; }
    // Command ids that were requested but have no handler, with counts (for the end-of-run report).
    std::map<uint32_t, uint64_t> unhandled;
    uint64_t requests = 0;

protected:
    struct Cmd { const char* name; Handler fn; };
    std::map<uint32_t, Cmd> cmds_;
    std::string name_;
};

// A stub: every command is logged ("STUB SERVICE <name> cmd <n> ...") and returns kernel::kStubService.
class StubService : public Service {
public:
    explicit StubService(std::string name) : Service(std::move(name)) {}
    bool is_stub() const override { return true; }
    uint32_t handle(Request& req, Response& rsp) override;
};

// ---- registry ---------------------------------------------------------------------------------------------------------------
using ServiceFactory = std::function<std::shared_ptr<Service>()>;
void register_service(const std::string& name, ServiceFactory f);   // replaces an earlier registration of the same name
bool has_service(const std::string& name);                           // a real (non-stub) registration exists
// Instantiates the service for `name`; an unregistered name yields a StubService (logged once per name). "sm:" is built in.
std::shared_ptr<Service> make_service(const std::string& name);
std::vector<std::string> service_names();                            // registered real services
std::vector<std::string> stub_names_requested();                     // names that were asked for and answered by a stub
void registry_reset();                                               // tests: forget registrations and counters, keep the built-in sm:

// The service manager as an object (also reachable through connect_named_port("sm:")).
// CMIF commands: 0 RegisterClient (pid), 1 GetServiceHandle (u64 name) -> move handle, 2 RegisterService, 3 UnregisterService,
// 4 DetachClient. TIPC variants: command ids are the same (message type - 16).
// RegisterService / UnregisterService answer kStubService (a guest-provided service is not supported) and log.
std::shared_ptr<Service> make_sm();

// ---- sessions ------------------------------------------------------------------------------------------------------------
struct DomainState {
    std::map<uint32_t, std::shared_ptr<Service>> objects;
    uint32_t next_object = 1;
};
struct KSession : kernel::KObject {
    std::shared_ptr<Service> svc;
    bool domain = false;
    std::shared_ptr<DomainState> dom;                 // shared by clones
    kernel::ObjType type() const override { return kernel::ObjType::Session; }
    ~KSession() override;                             // calls svc->on_close() when this was the last reference
};

// svcConnectToNamedPort: "sm:" gives the service manager; any other registered/stub name its service. Result code; handle in *out.
uint32_t connect_named_port(const char* name, uint32_t* out_handle);
uint32_t create_session_handle(std::shared_ptr<Service> svc);        // new KSession handle for a service object (used by sm GetServiceHandle)
// svcSendSyncRequest / ...WithUserBuffer: reads the request message (`msg_size` bytes at msg_addr; 0x100 for the TLS buffer), runs it,
// writes the response over the same buffer. Returns the SVC result code (0 on any answered request, even one carrying an error
// Result inside the CMIF header; kInvalidHandle for a bad session handle; 0xF601 for a bad domain object).
uint32_t send_sync_request(::Ctx* c, uint32_t session_handle, uint64_t msg_addr, uint64_t msg_size);

struct ServiceStats {
    std::string name;
    uint64_t requests = 0;
    bool stub = false;
    std::map<uint32_t, uint64_t> unhandled;
};
std::vector<ServiceStats> service_stats();                           // per service name, accumulated over sessions
std::string ipc_summary();                                           // one-line-per-service end-of-run summary text

}  // namespace nxrt::ipc
