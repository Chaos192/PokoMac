// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// RyuLDN: local wireless through a Ryujinx LDN master server ([net] protocol = ryuldn, services/ldn_ryu.cpp), so AnySwitch players host
// and join the rooms Ryujinx players see on that server (Ryujinx's online local wireless). Game-independent. One TCP connection per ldn:u
// object; the master relays every message and every game datagram between the members of a room.
//
// Ported from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors), src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LdnRyu:
// RyuLdnProtocol.cs (framing), LdnMasterProxyClient.cs (the client's behaviour and timeouts), Types/*.cs (the messages),
// Proxy/LdnProxy.cs and Proxy/LdnProxySocket.cs (a game datagram as ProxyData); UserServiceCreator/Types/*.cs (the requests);
// Ldn/Types/*.cs (the nn::ldn structs inside them). The layouts are ported exactly; the code is our own.
//
// Layout evidence: besides the C# declarations, the sizes and field offsets .NET itself gives Ryujinx's struct files (Unsafe.SizeOf and
// field addresses: the managed layout MemoryMarshal.Read and SpanHelpers.AsSpan use on both ends), measured with .NET 10 x64 on the
// unmodified files. Three differ from their declared StructLayout Size, because the CLR uses the larger of Size and the fields' extent:
// LdnHeader 0xC (declared 0xA: the int DataSize is 4-aligned at 8), ConnectRequest 0x500 (declared 0x4FC: the 8-aligned NetworkInfo
// sits at 0x80) and ConnectPrivateRequest 0xC0 (declared 0xBC: the 8-aligned NetworkConfig sits at 0xA0). Every other one is its
// declared size. Every integer is little-endian; IPv4 addresses are host-order u32 (10.114.0.1 = 0x0A720001, LdnProxy.cs:33-35,152-163).
//
// Frame (RyuLdnProtocol.cs): header 0xC = u32 magic 'R' 'L' 'D' 'N' (:13), u8 type, u8 version 1 (:12), 2 bytes padding, s32 size of the
// body that follows (LdnHeader.cs). A wrong magic or version, or a frame of 131072 bytes or more, ends the connection (:88-103).
// Messages (type: body; c = client, s = server):
//   0 Initialize      c<->s  InitializeMessage 0x16: 16-byte id, 6-byte MAC. The client sends what the server gave this process last
//                            time (zeros at first; LdnMasterProxyClient.cs:25,114-117), the server answers with this client's (:182-185).
//   1 Passphrase      c->s   0x80 bytes, UTF-8, NUL-terminated (:129). Sent on every connection before Initialize (OnConnected,
//                            :133-140). Empty: the public rooms; private: "Ryujinx-" + 8 lowercase hex digits (Ryujinx's settings check,
//                            Ryujinx/UI/ViewModels/SettingsViewModel.cs:640-643, Ryujinx.Common/Helpers/Patterns.cs:118).
//   2 CreateAccessPoint        c->s  CreateAccessPointRequest 0xBC (Pack 1), then the advertise data.
//   3 CreateAccessPointPrivate c->s  CreateAccessPointPrivateRequest 0x13C (Pack 1), then the advertise data.
//   4..6 ExternalProxy / ExternalProxyToken / ExternalProxyState: P2P, not in v1 (below).
//   7 SyncNetwork     s->c   NetworkInfo 0x480: the room changed.           15 Connected  s->c  NetworkInfo: this node is in the room.
//   8 Reject          c->s   RejectRequest 0x8 (u32 node id, u32 DisconnectReason) from the host; s->c the same to the node turned out.
//   9 RejectReply     s->c   no body.
//  10 Scan            c->s   ScanFilter 0x60.    11 ScanReply  s->c  NetworkInfo, one per room.    12 ScanReplyEnd  s->c  no body.
//  13 Connect         c->s   ConnectRequest 0x500.    14 ConnectPrivate  c->s  ConnectPrivateRequest 0xC0.
//  16 Disconnect      c<->s  DisconnectMessage 0x4 (u32 ip; 0 from the client, :313): leaving / the room ended.
//  17 ProxyConfig     s->c   u32 ip, u32 mask (ProxyConfig.cs): this node's virtual IPv4 address on the room's network.
//  18 ProxyConnect / 19 ProxyConnectReply / 21 ProxyDisconnect: TCP between nodes (v2, below).
//  20 ProxyData       c<->s  ProxyDataHeader 0x14 = ProxyInfo 0x10 + u32 length, then the bytes. ProxyInfo (Pack 1): u32 source ip @0,
//                            u16 source port @4, u32 destination ip @6, u16 destination port @0xA, s32 protocol @0xC (ProtocolType: 17 UDP,
//                            6 TCP). The master routes it to the destination member, or to every other member for the subnet broadcast.
//  22 SetAcceptPolicy c->s   u8 AcceptPolicy.       23 SetAdvertiseData  c->s  the advertise data.
// 254 Ping            c<->s  u8 requester, u8 id: requester 0 = the server asks, the client sends it back as it came (:214-222).
// 255 NetworkError    s->c   s32 NetworkError (NetworkError.cs); PortUnreachable is about P2P only (:224-234).
// Client behaviour (LdnMasterProxyClient.cs): the connection is made on the first Scan / CreateNetwork / Connect (EnsureConnected,
// :101-120, 4 s); Scan waits 1 s for ScanReplyEnd, none = no rooms (:548-577); CreateNetwork / Connect wait 4 s for Connected (:448-471,
// :579-620; a NetworkError ends the wait here at once, the answer is the same); Reject waits 6 s for RejectReply (:319-336); a connection in
// no room closes after 6 s without a Scan (NetworkTimeout, InactiveTimeout :27, refreshed by a Scan outside a room and on leaving one).
//
// The game's datagrams (link()): a vnet::Link on the ProxyConfig address and mask; every datagram is ProxyData (UDP) through the master.
// Connect and CreateNetwork also wait (inside the same 4 s) for the ProxyConfig, so GetIpv4Address is right when they return.
//
// v1 leaves out (TODO v2):
//   - TCP between nodes: ProxyConnect / ProxyConnectReply / ProxyData with protocol TCP / ProxyDisconnect (Proxy/LdnProxySocket.cs). A game
//     stream socket's connect to a peer answers ECONNREFUSED at once (the link answers vsock's vtcp SYN with an RST, vsock.h); a peer's
//     ProxyConnect is ignored (logged once).
//   - P2P: hosting a P2pProxyServer behind UPnP (ConfigureAccessPoint, :375-446; RyuNetworkConfig's proxy fields stay 0, so the master
//     relays all of a room we host), and following an ExternalProxy (:187-212) to a host's P2P proxy: a Connect the master answers that
//     way gets no ProxyConfig and fails (ConnectFailure, logged), so a Ryujinx host whose P2P proxy is reachable cannot be joined yet.
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tcp.h"
#include "vnet.h"

namespace nxrt::vnet::ryuldn {

constexpr uint16_t kDefaultPort = 30456;                    // Ryujinx.Common/SharedConstants.cs:6 LanPlayPort
constexpr uint32_t kMagic = 0x4E444C52u;                    // 'R' | 'L' << 8 | 'D' << 16 | 'N' << 24 (RyuLdnProtocol.cs:13)
constexpr uint8_t kVersion = 1;                             // RyuLdnProtocol.cs:12
constexpr size_t kMaxPacket = 131072;                       // RyuLdnProtocol.cs:14 (header + body must stay below it, :100)
constexpr int kFailureTimeoutMs = 4000, kScanTimeoutMs = 1000, kInactiveTimeoutMs = 6000;   // LdnMasterProxyClient.cs:27-29
constexpr int32_t kProtoTcp = 6, kProtoUdp = 17;            // System.Net.Sockets.ProtocolType (ProxyInfo.Protocol)

enum class Packet : uint8_t {   // PacketId.cs
    Initialize = 0, Passphrase, CreateAccessPoint, CreateAccessPointPrivate, ExternalProxy, ExternalProxyToken, ExternalProxyState,
    SyncNetwork, Reject, RejectReply, Scan, ScanReply, ScanReplyEnd, Connect, ConnectPrivate, Connected, Disconnect,
    ProxyConfig, ProxyConnect, ProxyConnectReply, ProxyData, ProxyDisconnect, SetAcceptPolicy, SetAdvertiseData,
    Ping = 254, NetworkError = 255,
};

enum class NetError : int32_t {   // UserServiceCreator/Types/NetworkError.cs
    None = 0, PortUnreachable, TooManyPlayers, VersionTooLow, VersionTooHigh, ConnectFailure, ConnectNotFound, ConnectTimeout,
    ConnectRejected, RejectFailed, Unknown = -1,
};

// ---- messages -------------------------------------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct Header {                // LdnHeader.cs (managed layout 0xC)
    uint32_t magic;
    uint8_t type;
    uint8_t version;
    uint8_t pad[2];
    int32_t data_size;
};
struct InitializeMessage {     // Types/InitializeMessage.cs
    uint8_t id[16];
    uint8_t mac[6];
};
struct PingMessage {           // Types/PingMessage.cs
    uint8_t requester;
    uint8_t id;
};
struct DisconnectMessage {     // Types/DisconnectMessage.cs
    uint32_t disconnect_ip;
};
struct NetworkErrorMessage {   // UserServiceCreator/Types/NetworkErrorMessage.cs
    int32_t error;
};
struct ProxyConfig {           // UserServiceCreator/Types/ProxyConfig.cs
    uint32_t proxy_ip;
    uint32_t proxy_subnet_mask;
};
struct ProxyInfo {             // Types/ProxyInfo.cs (Pack 1)
    uint32_t source_ip;
    uint16_t source_port;
    uint32_t dest_ip;
    uint16_t dest_port;
    int32_t protocol;
};
struct ProxyDataHeader {       // Types/ProxyDataHeader.cs
    ProxyInfo info;
    uint32_t data_length;
};
struct RejectRequest {         // Types/RejectRequest.cs
    uint32_t node_id;
    uint32_t disconnect_reason;
};
struct RyuNetworkConfig {      // Types/RyuNetworkConfig.cs (Pack 1); v1 sends the game version only (no P2P proxy: the rest 0)
    uint8_t game_version[16];
    uint8_t private_ip[16];
    int32_t address_family;
    uint16_t external_proxy_port;
    uint16_t internal_proxy_port;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 0xC && offsetof(Header, type) == 4 && offsetof(Header, version) == 5 && offsetof(Header, data_size) == 8);
static_assert(sizeof(InitializeMessage) == 0x16 && sizeof(PingMessage) == 2 && sizeof(DisconnectMessage) == 4);
static_assert(sizeof(NetworkErrorMessage) == 4 && sizeof(ProxyConfig) == 8 && sizeof(RejectRequest) == 8);
static_assert(sizeof(ProxyInfo) == 0x10 && offsetof(ProxyInfo, dest_ip) == 6 && offsetof(ProxyInfo, dest_port) == 0xA &&
              offsetof(ProxyInfo, protocol) == 0xC);
static_assert(sizeof(ProxyDataHeader) == 0x14 && offsetof(ProxyDataHeader, data_length) == 0x10);
static_assert(sizeof(RyuNetworkConfig) == 0x28 && offsetof(RyuNetworkConfig, address_family) == 0x20 &&
              offsetof(RyuNetworkConfig, external_proxy_port) == 0x24);
constexpr size_t kPassphraseSize = 0x80;   // Types/PassphraseMessage.cs

// The requests, as byte offsets (their nn::ldn parts: SecurityConfig 0x44, SecurityParameter 0x20, UserConfig 0x30, NetworkConfig 0x20,
// AddressList 0x60, NetworkInfo 0x480, ScanFilter 0x60; Ldn/Types/*.cs).
namespace create_ap {          // UserServiceCreator/Types/CreateAccessPointRequest.cs (Pack 1); the advertise data follows
constexpr size_t kSecurity = 0, kUser = 0x44, kNetConfig = 0x74, kRyuConfig = 0x94, kSize = 0xBC;
}
namespace create_ap_private {  // CreateAccessPointPrivateRequest.cs (Pack 1); the advertise data follows
constexpr size_t kSecurity = 0, kSecParam = 0x44, kUser = 0x64, kNetConfig = 0x94, kAddressList = 0xB4, kRyuConfig = 0x114, kSize = 0x13C;
}
namespace connect_req {        // ConnectRequest.cs (managed 0x500)
constexpr size_t kSecurity = 0, kUser = 0x44, kVersion = 0x74, kOption = 0x78, kNetworkInfo = 0x80, kSize = 0x500;
}
namespace connect_private {    // ConnectPrivateRequest.cs (managed 0xC0)
constexpr size_t kSecurity = 0, kSecParam = 0x44, kUser = 0x64, kVersion = 0x94, kOption = 0x98, kNetConfig = 0xA0, kSize = 0xC0;
}
constexpr size_t kInfoSize = 0x480, kScanFilterSize = 0x60;

using Info = std::array<uint8_t, kInfoSize>;   // nn::ldn NetworkInfo as the server sends it

// ---- framing --------------------------------------------------------------------------------------------------------------------------------
// Header + body + extra (Encode(type, packet, data), RyuLdnProtocol.cs:365-378).
std::vector<uint8_t> encode(Packet t, const void* body = nullptr, size_t n = 0, const void* extra = nullptr, size_t extra_n = 0);
// The frame at the start of b: 0 when it is not complete yet, -1 when it is malformed (magic, version, size), else its size.
int parse(const uint8_t* b, size_t n, Header* h);
// "" (public rooms) or "Ryujinx-" + 8 lowercase hex digits.
bool valid_passphrase(const std::string& p);
// [net] ryuldn_server: host or host:port (default port kDefaultPort). False when it is empty or the port is not 1..65535.
bool parse_server(const std::string& s, std::string* host, uint16_t* port);

// ---- the client -----------------------------------------------------------------------------------------------------------------------------
class ProxyLink;

class Client : public std::enable_shared_from_this<Client> {
public:
    struct Events {
        std::function<void()> changed;                 // Connected / SyncNetwork: info() is new (the reader thread)
        std::function<void(uint32_t reason)> left;     // the room ended under this node: the DisconnectReason a Reject gave, 6 (SignalLost)
                                                       // when the connection to the server was lost, else 0 (a Disconnect from the server)
    };
    // game_version: RyuNetworkConfig.GameVersion (the title's display version; SetGameVersion, :347-355). Nothing is sent until the first
    // operation.
    static std::shared_ptr<Client> make(std::string host, uint16_t port, std::string passphrase, std::string game_version, Events ev);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    // Leaves the room (Disconnect), closes the connection, joins the threads; no event after it returns.
    void close();

    // Blocking. Never call these with a lock an event handler takes.
    // CreateNetwork: req is a CreateAccessPointRequest (0xBC) or, priv, a CreateAccessPointPrivateRequest (0x13C) with its RyuNetworkConfig
    // left to this call. None: the room is up (Connected and ProxyConfig came). Unknown: no connection to the server.
    NetError create(bool priv, std::vector<uint8_t> req, const std::vector<uint8_t>& adv);
    // Scan: the rooms the server lists for this ScanFilter (0x60); none on a timeout or a lost connection.
    std::vector<Info> scan(const uint8_t* filter);
    // Connect: req is a ConnectRequest (0x500) or, priv, a ConnectPrivateRequest (0xC0). None: in the room. Else the server's NetworkError,
    // ConnectTimeout (no Connected in 4 s), ConnectFailure (no ProxyConfig: an ExternalProxy room, v2) or Unknown (no connection).
    NetError connect(bool priv, const std::vector<uint8_t>& req);
    void leave();                                      // Disconnect from the room (no event)
    bool reject(uint32_t node_id, uint32_t reason);    // false: no RejectReply in 6 s, or the server answered an error
    void set_accept_policy(uint8_t policy);            // only while in a room (the server keeps it per room)
    void set_advertise_data(const std::vector<uint8_t>& adv);

    bool in_network() const;
    Info info() const;                                 // the room as the server last sent it (zeros outside a room)
    ProxyConfig proxy() const;                         // zeros outside a room
    std::shared_ptr<Link> link() const;                // the game's datagrams (vnet::set_current_link while in a room)

    bool send_packet(Packet t, const void* body = nullptr, size_t n = 0, const void* extra = nullptr, size_t extra_n = 0);

private:
    Client(std::string host, uint16_t port, std::string passphrase, std::string game_version, Events ev);
    bool ensure_connected();
    void read_loop(tcp::Handle h);
    void handle(const Header& h, const std::vector<uint8_t>& body);
    void lost(uint32_t reason);                        // DisconnectInternal (:286-307): the room ended under us
    void timer_loop();
    void refresh_idle_locked();                        // caller holds mu_
    NetError wait_joined(std::unique_lock<std::mutex>& lk, uint64_t seq0, uint64_t pseq0);

    const std::string host_;
    const uint16_t port_;
    const std::string pass_;
    std::array<uint8_t, 16> game_version_{};
    const Events ev_;

    std::mutex conn_mu_;                    // one ensure_connected at a time
    std::mutex send_mu_;                    // writes; sock_ under it
    tcp::Handle sock_ = tcp::kBad;          // what send_packet writes to
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> rd_stop_{false};      // the reader ends the connection (within kReaderTickUs) and closes its socket
    bool up_ = false, closing_ = false;
    std::thread reader_, timer_;
    bool in_net_ = false;
    Info info_{};
    ProxyConfig proxy_{};
    uint64_t connected_seq_ = 0, proxy_seq_ = 0;
    bool ext_proxy_ = false;
    std::vector<Info> scan_;
    bool scan_done_ = false, reject_reply_ = false;
    NetError last_error_ = NetError::None;
    uint32_t reject_reason_ = 0;
    int64_t idle_at_ = 0;                   // steady ms the idle connection closes at (0: never)
    bool logged_tcp_ = false;
    std::shared_ptr<ProxyLink> link_;
};

// A vnet::Link on the ProxyConfig network: send() is ProxyData (UDP) through the client, ProxyData from the server is delivered. A vtcp
// segment (ports 0 and 0, vsock.h) is answered with an RST at once (TCP between nodes is v2), so a stream connect to a peer is refused.
class ProxyLink final : public Link {
public:
    explicit ProxyLink(std::weak_ptr<Client> c) : client_(std::move(c)) {}
    uint32_t local_ip() const override;
    bool send(const Datagram& d) override;
    uint32_t netmask() const override;
    bool reaches(uint32_t dst) const override;

    void configure(uint32_t ip, uint32_t mask);   // ProxyConfig
    void down();                                   // the room ended: local_ip() 0, sends fail
    void from_wire(const ProxyDataHeader& h, const uint8_t* p, size_t n);

private:
    void refuse_vtcp(const Datagram& d);
    std::weak_ptr<Client> client_;
    mutable std::mutex mu_;
    uint32_t ip_ = 0, mask_ = 0;
    bool up_ = false;
};

}  // namespace nxrt::vnet::ryuldn
