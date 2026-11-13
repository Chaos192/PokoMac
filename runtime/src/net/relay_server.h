// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// The relay server inside the runtime ([net] server = auto: the game that hosts runs it), and LAN discovery of relays. Game-independent.
//
//   RelayServer     a C++ port of the launcher's relay (launcher/src-tauri/src/relay/server.rs, protocol v3 in relay/mod.rs): the same
//                   frames, room rules and UDP datagram path, so the launcher's relay, this one and every client interoperate. HELLO
//                   proto 2..3. Limits as the launcher's: 32 connections, 64 rooms, 8 nodes a room, 2000 frames/s a client (ERROR 9 +
//                   close), 15 s keepalive, 256 queued frames / 5 s write timeout a client, UDP packets only from the token's TCP peer IP
//                   and at most 2000/s a token. Room codes: a room with a key is listed to and joined with that key only; 5 wrong codes on
//                   one connection or 30 from one address in 10 minutes: ERROR 9 + close (the launcher's per-address rule, without its
//                   "first fruitless keyed scan" count). No build stamp check (the launcher's relay has none by default).
//                   Threads: accept, UDP, and per client a reader and a writer (bounded queue); one lock over the rooms and clients,
//                   nothing waits on a socket while holding it (UDP sendto aside). Winsock only (runs under Wine/Proton as well).
//   LAN discovery   on the relay's UDP port. Request (64 B): "PKPDISC1" + 8 B nonce + zeros. Reply, only to a request from a private,
//                   link-local or loopback IPv4 address and never longer than the request: "PKPHOST1" + the nonce + u16 TCP port + u8
//                   open rooms (no room key, not full) + u8 nickname length + the host's nickname (<= 32 B). The launcher's relay answers
//                   it too (nickname "Relay").
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lobby.h"

namespace nxrt::vnet {

struct RelayServerOptions {
    uint16_t port = kRelayDefaultPort;   // TCP and UDP
    uint32_t listen_ip = 0;              // host order; 0 = every address (0.0.0.0). 127.0.0.1: this PC only (tests; nothing for a
                                         // firewall to ask about), a LAN address: that network only
    std::string password;                // empty: none
    std::string nickname = "Relay";      // the discovery reply's host name
    std::string server_version = "nxrt relay";
    bool udp = true;                     // the UDP socket (datagram path and discovery); a failed UDP bind costs only those
    int keepalive_ms = 15000;
};

class RelayServer {
public:
    // Binds and starts. nullptr with a short reason in *why on failure; *in_use: the TCP port is taken (another relay on this PC).
    static std::unique_ptr<RelayServer> start(const RelayServerOptions& opt, std::string* why, bool* in_use);
    ~RelayServer();   // stop()
    // LEFT 3 to every room member, closes every connection, joins every thread.
    void stop();
    uint16_t port() const;
    bool udp() const;   // the UDP socket is open
    struct Impl;

private:
    explicit RelayServer(std::unique_ptr<Impl> im);
    std::unique_ptr<Impl> im_;
};

// ---- LAN discovery ---------------------------------------------------------------------------------------------------------------------
constexpr size_t kDiscoveryLen = 64;

struct FoundRelay {
    uint32_t ip = 0;       // host order, the reply's source address
    uint16_t port = 0;     // the relay's TCP port
    uint8_t open_rooms = 0;
    std::string nickname;
};

// Broadcasts a request to 255.255.255.255:port, every up IPv4 interface's directed broadcast and 127.0.0.1:port, and collects replies
// for wait_ms (the whole window), one entry per address:port.
// bind_ip (host order): the request socket's address; 0 = every address. On the loopback (127.0.0.1) only a relay on this machine is
// asked: nothing listens beyond it (a test on a PC whose firewall must not ask).
std::vector<FoundRelay> discover_relays(uint16_t port, int wait_ms, uint32_t bind_ip = 0);

struct LocalAddr {
    uint32_t ip = 0, mask = 0;   // host order
    std::string adapter;         // the friendly name
};
// This PC's IPv4 addresses on adapters that are up, loopback left out.
std::vector<LocalAddr> local_ipv4();

// 10/8, 172.16/12, 192.168/16, 169.254/16, 127/8 (host order).
constexpr bool is_lan_ipv4(uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE || (a >> 24) == 127;
}

}  // namespace nxrt::vnet
