// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The request recorder ([ipc] record, src/ipc/record.cpp): the lines a replay (tests/ipc_replay, tools/services/ipc_diff) needs, the
// objects a reply hands out numbered so later requests name them, and a service's close.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ipc.h"
#include "svc_test.h"

using namespace svctest;

namespace {
bool has_line(const std::vector<std::string>& v, const std::string& start, const std::string& part) {
    for (const std::string& l : v)
        if (l.rfind(start, 0) == 0 && l.find(part) != std::string::npos) return true;
    return false;
}
}  // namespace

SVC_TEST(ipc_record, lines) {
    reset();
    const std::filesystem::path file = std::filesystem::path(nxrt::config_get_str("fs", "save_dir")).parent_path() / "ipc_record.txt";
    CHECK(nxrt::ipc::record::begin(file.string()));
    Session pl = open("pl:u");
    CHECK(pl.valid());
    pl.call(2, Req().put<uint32_t>(0));                  // GetSize(0)
    pl.call(4);                                           // GetSharedMemoryNativeHandle: a shared memory handle
    Session acc = open("acc:u0");
    const Reply user = acc.call(4);                       // GetLastOpenedUser
    const Reply prof = acc.call(5, Req().put(user.get<uint64_t>(0)).put(user.get<uint64_t>(8)));   // GetProfile -> IProfile
    CHECK_EQ(prof.objects.size(), 1u);
    Session p = acc.object(prof);
    p.call(1);                                            // IProfile::GetBase
    p.close();
    nxrt::ipc::record::end();

    std::vector<std::string> v;
    std::ifstream in(file);
    for (std::string l; std::getline(in, l);) v.push_back(l);
    CHECK(!v.empty() && v[0].rfind("H nxrt-ipc-record 1 program=", 0) == 0);
    CHECK(has_line(v, "Q ", " sm: 1 "));                 // GetServiceHandle on the service manager
    CHECK(has_line(v, "A ", " o=2:pl:u"));               // pl:u is the first object handed out (sm: is sid 1)
    CHECK(has_line(v, "Q ", " 2 pl:u 2 4 0 in=00000000"));
    CHECK(has_line(v, "A ", " hc=s"));                   // the font memory: its size and contents hash
    CHECK(has_line(v, "A ", ":"));
    // the profile object gets the next sid; its request names it; closing its last session writes X
    std::string sid;
    for (const std::string& l : v)
        if (l.rfind("Q ", 0) == 0 && l.find(" acc:u0 5 ") != std::string::npos) sid = "?";
    for (const std::string& l : v) {
        const size_t o = l.find(" o=");
        if (l.rfind("A ", 0) == 0 && o != std::string::npos && l.find("IProfile", o) != std::string::npos)
            sid = l.substr(o + 3, l.find(':', o) - o - 3);
    }
    CHECK(!sid.empty() && sid != "?");
    CHECK(has_line(v, "Q ", " " + sid + " "));
    CHECK(has_line(v, "X " + sid, ""));
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
