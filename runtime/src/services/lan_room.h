// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// LAN room: a game's own LAN mode (Pia's "LAN" sessions: UDP broadcast browse on port 35000, session traffic on 49152..49155; Kinnay,
// NintendoClients wiki "LAN Protocol") played over the internet. Every player of one title with the same room code is put on one virtual
// LAN: a room of the Link Play relay (runtime/src/net/lobby.h), whose datagram link carries the bsd UDP sockets (vsock) on 169.254.R.N/24,
// broadcasts included. nifm reports that network while the room is joined, so the game's local-network request is accepted and its LAN
// mode finds the other players. No game server, no account, nothing of Nintendo's: only the relay. Game-independent.
//
// [net] lan = off (default) | room. With room: [net] server (host:port, or auto = a relay on this network found by LAN discovery, else the
// relay inside this process on port 39660), [net] room_code (empty: the open LAN room of the title; a code: only players with that code),
// [net] password, [net] udp, [net] host_listen as for Link Play (docs/multiplayer/README.md). One relay room per title and code:
// local communication id = the application id (the program id with its low 12 bits clear), scene kLanScene, advertise data kLanAdvert. The first player to arrive creates it, the others join
// it; two that create at once settle on the lower room id. The room closes when the player who created it leaves (the relay's rule), and the
// others then go back to no network and join or create again at the game's next network request.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace nxrt::services {

// nifm's network state (services/network.cpp nifm_now): a source that reports a LAN the game is on. It answers true with this node's
// address and mask (host order) while that LAN is up; nifm then reports the connection on that address. Called at every nifm query and
// at each request's Submit, on the game's threads. Null (the default): no such source. (AnySwitch declares it in net_redirect.h.)
using NifmLanSource = bool (*)(uint32_t* ip, uint32_t* mask);
void nifm_set_lan_source(NifmLanSource f);

}  // namespace nxrt::services

namespace nxrt::services::lanroom {

constexpr uint16_t kLanScene = 0xFFFE;                    // the scene id of LAN rooms (a game's own local wireless rooms never use it)
constexpr int16_t kLanVersion = 1;                        // the LAN room protocol version (the relay refuses another)
constexpr char kLanAdvert[] = "AnySwitch LAN room 1";
constexpr char kLanPass[] = "AnySwitch LAN room";

// [net] lan = room (read on every call: tests change it).
bool enabled();

// While enabled: joins the title's LAN room, or creates it, when not in it yet (blocking: the relay's answers, a few seconds at most).
// True when in the room afterwards. Disabled: false at once.
bool join_now();

// nifm's LAN source (registered with services::nifm_set_lan_source): the room address and its mask (host order) while in the room. When
// not in it: the first call, and the first call after the room was left, join synchronously (the game's network request waits for the
// answer, as SubmitNetworkRequestAndWait does on a console); after a failure the next attempts run in the background, 3 s apart doubling
// to 60 s, and the call answers false at once. Disabled: false at once.
bool source(uint32_t* ip, uint32_t* mask);

// The room address and its mask (host order) while in the room; false otherwise. Never joins.
bool ip_config(uint32_t* ip, uint32_t* mask);

// The relay's room id while in the room, else 0 (logs and tests).
uint32_t room_id();

// Leaves the room (if in one) and forgets the relay client: services::network_reset() and tests.
void reset();

}  // namespace nxrt::services::lanroom
