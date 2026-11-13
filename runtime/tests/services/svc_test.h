// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Service tests: requests go through the runtime's own dispatch (ipc::send_sync_request on a session handle from sm:), as the SDK's
// client code sends them, and the reply is parsed back from the message words. Each family file registers its cases with SVC_TEST.
// Guest pointers are host pointers, so a buffer is any host memory.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"

namespace svctest {

namespace hipc = nxrt::hipc;

// ---- registration and checks ------------------------------------------------------------------------------------------------
struct Case {
    const char* family;
    const char* name;
    void (*fn)();
};
std::vector<Case>& cases();
struct Reg {
    Reg(const char* family, const char* name, void (*fn)()) { cases().push_back({family, name, fn}); }
};
#define SVC_TEST(family, name)                                                     \
    static void svc_test_##family##_##name();                                    \
    static ::svctest::Reg svc_reg_##family##_##name(#family, #name, &svc_test_##family##_##name); \
    static void svc_test_##family##_##name()

void fail(const char* file, int line, const std::string& what);
extern int g_checks;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++::svctest::g_checks;                                          \
        if (!(cond)) ::svctest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                   \
    do {                                                                                                                 \
        ++::svctest::g_checks;                                                                                           \
        const auto va_ = (a);                                                                                            \
        const auto vb_ = (b);                                                                                            \
        if (!(va_ == vb_))                                                                                               \
            ::svctest::fail(__FILE__, __LINE__,                                                                          \
                            std::string("CHECK_EQ(" #a ", " #b "): ") + std::to_string((long long)va_) + " != " +        \
                                std::to_string((long long)vb_));                                                          \
    } while (0)

// ---- requests ---------------------------------------------------------------------------------------------------------------
struct Buf {   // a buffer the request describes: kind 'A' send, 'B' receive, 'X' pointer, 'C' receive list
    char kind;
    void* p;
    uint64_t size;
};
inline Buf A(const void* p, uint64_t n) { return {'A', const_cast<void*>(p), n}; }
inline Buf B(void* p, uint64_t n) { return {'B', p, n}; }
inline Buf X(const void* p, uint64_t n) { return {'X', const_cast<void*>(p), n}; }
inline Buf C(void* p, uint64_t n) { return {'C', p, n}; }

struct Req {
    std::vector<uint8_t> raw;     // the arguments after the SFCI header
    std::vector<Buf> bufs;
    std::vector<uint32_t> copy;   // copy handles sent
    std::vector<uint32_t> objs;   // domain object ids (or, outside a domain, session handles moved) sent as interfaces
    bool pid = false;
    template <typename T>
    Req& put(const T& v) {   // appended at its natural alignment, as the SDK's CMIF writer lays raw arguments out
        const size_t a = sizeof(T) >= 8 ? 8 : sizeof(T) >= 4 ? 4 : sizeof(T) >= 2 ? 2 : 1;
        raw.resize((raw.size() + a - 1) / a * a);
        const size_t o = raw.size();
        raw.resize(o + sizeof(T));
        std::memcpy(raw.data() + o, &v, sizeof(T));
        return *this;
    }
    Req& bytes(const void* p, size_t n) {
        const size_t o = raw.size();
        raw.resize(o + n);
        std::memcpy(raw.data() + o, p, n);
        return *this;
    }
    Req& buf(Buf b) {
        bufs.push_back(b);
        return *this;
    }
    Req& with_pid() {
        pid = true;
        return *this;
    }
};

struct Reply {
    bool ok = false;             // a reply message came back and parsed
    uint32_t svc_result = 0;     // what svcSendSyncRequest returned
    uint32_t result = 0;         // the CMIF result word
    std::vector<uint8_t> data;   // raw output (after SFCO), in whole words as the wire carries it
    std::vector<uint32_t> copy, move;
    std::vector<uint32_t> objects;   // domain object ids returned (domain sessions), else move handles that are sessions
    template <typename T>
    T get(size_t off) const {
        T v{};
        if (off + sizeof(T) <= data.size()) std::memcpy(&v, data.data() + off, sizeof(T));
        return v;
    }
    size_t words() const { return data.size() / 4; }
};

// A client session: plain or (after to_domain) a domain, with the objects it was given.
class Session {
public:
    Session() = default;
    explicit Session(uint32_t h, bool dom = false, uint32_t obj = 0) : handle_(h), domain_(dom), object_(obj) {}
    uint32_t handle() const { return handle_; }
    bool valid() const { return handle_ != 0; }
    bool domain() const { return domain_; }
    Reply call(uint32_t cmd, const Req& r = {}) const;
    // The interface returned at index i of a reply: a new Session (a domain object on this domain, or its own session handle).
    Session object(const Reply& rp, size_t i = 0) const;
    bool to_domain();   // ConvertCurrentObjectToDomain
    void close();

private:
    uint32_t handle_ = 0;
    bool domain_ = false;
    uint32_t object_ = 0;   // domain object id (domain sessions)
};

// sm:GetServiceHandle(name): a session on the named service. Invalid when sm refuses.
Session open(const char* name);

// Expected reply shapes: result 0 and exactly `bytes` of raw output (rounded up to words, as on the wire).
inline bool shape(const Reply& r, size_t bytes, size_t copy = 0, size_t objects = 0) {
    return r.ok && r.result == 0 && r.data.size() == (bytes + 3) / 4 * 4 && r.copy.size() == copy && r.objects.size() == objects;
}
#define CHECK_SHAPE(reply, ...)                                                                                                \
    do {                                                                                                                       \
        ++::svctest::g_checks;                                                                                                 \
        const ::svctest::Reply& r_ = (reply);                                                                                  \
        if (!::svctest::shape(r_, __VA_ARGS__))                                                                                \
            ::svctest::fail(__FILE__, __LINE__,                                                                                \
                            std::string("CHECK_SHAPE(" #reply ", " #__VA_ARGS__ "): result 0x") + ::svctest::hex(r_.result) + \
                                ", " + std::to_string(r_.data.size()) + " data bytes, " + std::to_string(r_.copy.size()) +     \
                                " copy handles, " + std::to_string(r_.objects.size()) + " objects");                           \
    } while (0)
std::string hex(uint32_t v);

// A result that is a real Horizon error: nonzero, not the runtime's own "no handler" stub value.
inline bool well_formed_error(const Reply& r) { return r.ok && r.result != 0 && r.result != nxrt::kernel::kStubService && r.data.empty(); }

// The process-wide state every case starts from (services::reset_all).
void reset();

}  // namespace svctest
