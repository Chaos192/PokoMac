// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Command extensions (include/nxrt/ipc.h): commands added to an interface by name from their own file. Service::handle looks here only
// for a command the interface's own table lacks.
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/ipc.h"

namespace nxrt::ipc {

namespace {
struct Ext {
    const char* name;
    ExtHandler fn;
};
struct Table {
    std::mutex mu;
    std::map<std::string, std::map<uint32_t, Ext>> by_iface;
};
Table& table() {
    static Table t;
    return t;
}
}  // namespace

void extend(const std::string& iface, uint32_t id, const char* cmd_name, ExtHandler h) {
    std::lock_guard<std::mutex> lk(table().mu);
    table().by_iface[iface][id] = Ext{cmd_name, std::move(h)};
}

bool find_extension(const std::string& iface, uint32_t id, ExtHandler* out) {
    std::lock_guard<std::mutex> lk(table().mu);
    auto it = table().by_iface.find(iface);
    if (it == table().by_iface.end()) return false;
    auto c = it->second.find(id);
    if (c == it->second.end()) return false;
    if (out) *out = c->second.fn;
    return true;
}

std::vector<std::pair<std::string, std::pair<uint32_t, const char*>>> extensions() {
    std::lock_guard<std::mutex> lk(table().mu);
    std::vector<std::pair<std::string, std::pair<uint32_t, const char*>>> v;
    for (const auto& i : table().by_iface)
        for (const auto& c : i.second) v.push_back({i.first, {c.first, c.second.name}});
    return v;
}

void extensions_clear() {
    std::lock_guard<std::mutex> lk(table().mu);
    table().by_iface.clear();
}

}  // namespace nxrt::ipc
