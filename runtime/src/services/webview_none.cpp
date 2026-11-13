// SPDX-License-Identifier: GPL-3.0-or-later
// PokoMac has no web view (AnySwitch's webview.cpp, its macOS and Windows hosts and the in-place package reader it uses are not taken):
// this is the part of include/nxrt/webview.h that AnySwitch's am.cpp and hid.cpp (by Chaos192, taken as they are) ask, answered for a
// build without one. view_enabled() and online_enabled() are false and no host is ever registered, so the web and offline applets
// answer with am.cpp's stand-in, as PokoMac's applets did before; nothing is ever loaded from the network.
#include <atomic>
#include <cctype>
#include <string>
#include <vector>

#include "nxrt/webview.h"

namespace nxrt::webview {

namespace {
std::atomic<bool> g_held{false};
}

bool view_enabled() { return false; }
bool online_enabled() { return false; }

std::string url_host(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    size_t start = scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t at = host.rfind('@');                 // user:password@host
    if (at != std::string::npos) host = host.substr(at + 1);
    if (!host.empty() && host[0] == '[') {             // [IPv6]:port
        const size_t close = host.find(']');
        host = close == std::string::npos ? host : host.substr(0, close + 1);
    } else {
        const size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
    }
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return host;
}

// Anything not plainly another site's counts as Nintendo's: no host, a placeholder, or "nintendo" anywhere in the host.
bool nintendo_url(const std::string& url) {
    const std::string host = url_host(url);
    return host.empty() || host.find('%') != std::string::npos || host.find("nintendo") != std::string::npos;
}

void set_host(Host*) {}
Host* host() { return nullptr; }

void set_input_held(bool held) { g_held.store(held, std::memory_order_relaxed); }
bool input_held() { return g_held.load(std::memory_order_relaxed); }
void feed_pad(uint64_t, int32_t, int32_t) {}

std::string html_folder() { return std::string(); }
bool find_page(const std::string&, const std::string&, Page*) { return false; }

std::vector<uint8_t> return_storage(uint32_t, uint32_t, uint32_t, const std::string&) { return {}; }

}  // namespace nxrt::webview
