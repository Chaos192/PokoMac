// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// net_status.h: the status file and the control file of the game's network rooms, for the launcher. Game-independent.
#include "net_status.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define NXRT_GETPID _getpid
#else
#include <unistd.h>
#define NXRT_GETPID getpid
#endif

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::services::net_status {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
constexpr int kMinGapMs = 250, kRefreshMs = 5000, kPollMs = 500;

int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

struct State {
    std::mutex mu;
    std::condition_variable cv;
    bool writer_on = false, watcher_on = false, dirty = false;
    std::string code, server;
    std::map<std::string, std::string> parts;          // name -> JSON object text
    std::vector<std::pair<std::string, Command>> handlers;
    uint64_t applied = 0;                               // the highest command number applied (or present when the watcher started)
    fs::file_time_type seen{};                          // the control file's last change already read
};

// The highest command number in the control file now (0: none, or no file).
uint64_t max_command(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    uint64_t m = 0;
    while (std::getline(f, line)) {
        unsigned long long n = 0;
        char cmd[32] = {};
        if (std::sscanf(line.c_str(), "%llu %31s", &n, cmd) == 2 && n > m) m = n;
    }
    return m;
}
State& st() {
    static State* s = new State;   // never destroyed: the threads may still run while the process exits
    return *s;
}

// pkp.log's folder (the game folder for pkp).
std::string folder() {
    const std::string log = log_path();
    const size_t cut = log.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : log.substr(0, cut + 1);
}

// Caller holds s.mu.
std::string render_locked(const State& s) {
    std::ostringstream o;
    o << "{\"version\":1,\"pid\":" << NXRT_GETPID() << ",\"updated_ms\":" << now_ms() << ",\"code\":" << json_str(s.code)
      << ",\"server\":" << json_str(s.server);
    for (const auto& [name, obj] : s.parts) o << "," << json_str(name) << ":" << obj;
    o << "}\n";
    return o.str();
}

bool write_atomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f.flush()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);   // replaces the old file; a reader holding it open on Windows makes this fail: the next write retries
    return !ec;
}

void writer_loop() {
    State& s = st();
    std::unique_lock<std::mutex> lk(s.mu);
    bool warned = false;
    for (;;) {
        s.cv.wait_for(lk, std::chrono::milliseconds(kRefreshMs), [&] { return s.dirty; });
        s.dirty = false;
        const std::string text = render_locked(s);
        lk.unlock();
        if (!write_atomic(status_path(), text) && !warned) {
            warned = true;
            NXLOG_W("net", "net status: cannot write %s (tried again at the next change)", status_path().c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kMinGapMs));
        lk.lock();
    }
}

// Caller holds s.mu.
void ensure_writer_locked(State& s) {
    if (s.writer_on) return;
    s.writer_on = true;
    std::thread(writer_loop).detach();
}

void watcher_loop() {
    State& s = st();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        const std::string path = control_path();
        std::error_code ec;
        const fs::file_time_type t = fs::last_write_time(path, ec);
        std::vector<std::string> todo;
        std::vector<Command> run;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            if (ec || t == s.seen) continue;
            s.seen = t;
            std::ifstream f(path, std::ios::binary);
            std::string line;
            while (std::getline(f, line)) {
                unsigned long long n = 0;
                char cmd[32] = {};
                if (std::sscanf(line.c_str(), "%llu %31s", &n, cmd) != 2 || n <= s.applied) continue;
                s.applied = n;
                todo.emplace_back(cmd);
            }
            for (const auto& h : s.handlers) run.push_back(h.second);
        }
        for (const std::string& c : todo) {
            NXLOG_I("net", "net control: \"%s\" from the launcher", c.c_str());
            for (const Command& h : run) h(c);
        }
    }
}

}  // namespace

std::string status_path() { return folder() + "net-status.json"; }
std::string control_path() { return folder() + "net-control.txt"; }

std::string json_str(const std::string& text) {
    std::string o = "\"";
    for (const unsigned char c : text) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += static_cast<char>(c);   // UTF-8 passes as it is
            }
        }
    }
    return o + "\"";
}

void set_part(const std::string& name, const std::string& json_object) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (json_object.empty()) {
        if (!s.parts.erase(name)) return;
    } else {
        auto it = s.parts.find(name);
        if (it != s.parts.end() && it->second == json_object) return;
        s.parts[name] = json_object;
    }
    ensure_writer_locked(s);
    s.dirty = true;
    s.cv.notify_all();
}

void set_common(const std::string& code, const std::string& server) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.code == code && s.server == server) return;
    s.code = code;
    s.server = server;
    if (!s.writer_on) return;   // written with the first part
    s.dirty = true;
    s.cv.notify_all();
}

void on_command(const std::string& owner, Command handler) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    bool replaced = false;
    for (auto& h : s.handlers)
        if (h.first == owner) {
            h.second = handler;
            replaced = true;
        }
    if (!replaced) s.handlers.emplace_back(owner, std::move(handler));
    if (!s.watcher_on) {
        s.watcher_on = true;
        std::error_code ec;
        s.seen = fs::last_write_time(control_path(), ec);   // what is there now is older than any command of this run
        if (ec) s.seen = {};
        s.applied = max_command(control_path());           // its lines (an earlier run's) are never applied
        std::thread(watcher_loop).detach();
    }
}

void flush() {
    State& s = st();
    std::string text;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        text = render_locked(s);
    }
    write_atomic(status_path(), text);
}

void reset_for_tests() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.parts.clear();
    s.handlers.clear();
    s.code.clear();
    s.server.clear();
    std::error_code ec;
    s.seen = fs::last_write_time(control_path(), ec);
    if (ec) s.seen = {};
    s.applied = max_command(control_path());
}

}  // namespace nxrt::services::net_status
