// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The [tweaks] keys of S46 (play_fastboot.h).
#include "play_fastboot.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace nxrt::play {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool to_bool(const std::string& v, bool* out) {
    const std::string l = lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") return *out = true, true;
    if (l == "0" || l == "false" || l == "no" || l == "off") return *out = false, true;
    return false;
}

}  // namespace

bool fastboot_apply(const std::string& key, const std::string& value, FastbootSettings* s, bool* known, std::string* msg) {
    *known = true;
    const std::string k = lower(key);
    auto set_bool = [&](bool* dst) {
        if (to_bool(value, dst)) return true;
        *msg = k + " must be 0 or 1, got '" + value + "'";
        return false;
    };
    if (k == "skip_intro") return set_bool(&s->skip_intro);
    if (k == "skip_boot_logos") return set_bool(&s->skip_boot_logos);
    if (k == "fast_menus") return set_bool(&s->fast_menus);
    if (k == "menu_speed") {
        errno = 0;
        char* end = nullptr;
        const long long x = value.empty() ? 0 : std::strtoll(value.c_str(), &end, 10);
        if (value.empty() || errno != 0 || end != value.c_str() + value.size() || x < 1 || x > 4) {
            *msg = "menu_speed must be 1..4, got '" + value + "'";
            return false;
        }
        s->menu_speed = static_cast<int>(x);
        return true;
    }
    if (k == "language") {
        s->language = lower(value) == "auto" ? std::string() : value;
        return true;
    }
    *known = false;
    return true;
}

void fastboot_format(const FastbootSettings& s, std::string* out) {
    auto kv = [out](const char* k, const std::string& v) { *out += std::string(k) + " = " + v + "\n"; };
    kv("skip_intro", s.skip_intro ? "1" : "0");
    kv("skip_boot_logos", s.skip_boot_logos ? "1" : "0");
    kv("fast_menus", s.fast_menus ? "1" : "0");
    kv("menu_speed", std::to_string(std::clamp(s.menu_speed, 1, 4)));
    kv("language", s.language.empty() ? "auto" : s.language);
}

double fastboot_menu_rate(const FastbootSettings& s) { return s.fast_menus ? static_cast<double>(std::clamp(s.menu_speed, 1, 4)) : 1.0; }

}  // namespace nxrt::play
