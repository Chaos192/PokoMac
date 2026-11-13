# Multiplayer (Link Play) — experimental

Pokopia's **Link Play** (local wireless, several players on one island) between PCs, Steam Decks and other handhelds. This port speaks
pokopia-native's co-op protocol, so you can also play with **pokopia-native** players.

## Before you start

- **Cross-play** works with pokopia-native players on game version **2.0.1**. Players on 1.0.5 cannot join (the game builds differ).
- **Not compatible with earlier Link Play builds of this port** (the old relay on port 39660). Everyone needs a build with this protocol.
- **Each player needs their own save.** The host refuses a visitor whose save it already has (error 9005, "Communication error"):
  two copies of one save cannot play together. Earlier builds gave every new save the same player id, so every visit failed with
  9005; the game now repairs such a save by itself at start (a copy is kept in `uuid-backup\` in the save folder).
- Every player needs the same **Interact everywhere** and **Story co-op** settings, or the host is not listed. Only the host's save keeps story progress.

## Setting it up

Use the launcher's **Link Play** page. It writes the `[net]` and `[pkp]` keys in `pkp.ini` for you; changes apply the next time the game
starts. Then in the game: the PC in your first house -> Link Play (unlocked after Environment Level 2 in Withered Wasteland) -> **Local
connection**. One player picks **Invite Others to Visit**, the others **Visit a Friend**.

| Choice | What it does |
|---|---|
| Alone | No Link Play. |
| Automatic (LAN) | The default. On the same Wi-Fi or network: the host is found by itself. |
| Visit by address | Join one host by its address (shown on the host's Link Play page). Use this when the host is not found. |
| Internet | Host or visit through a relay with a room code. The host's page shows the code; visitors type the relay address and the code. |

Options: **Interact everywhere** (visitors can use moves and items and build in the story areas too, not only in Palette Town) and **Story
co-op** (visitors can talk to the story characters; turns on Interact everywhere). Both are experimental.

**Steam Deck and other Proton/Linux devices** find hosts and are found the same way. Proton has no DNS-SD, so there the game sweeps
its own network for hosts on port 47600 each time it scans, and a hosting device answers DNS-SD (mDNS) queries itself, so Windows players
see it too. Nothing to type, and nothing to change when you take the device to another network. `[net] lan_sweep = off` turns this off.

What it cannot get past:
- **Wi-Fi that keeps devices apart** (many cafes, hotels and guest networks: "client isolation"). Nobody on it can reach anybody. Use
  **Internet** with a relay and a room code instead.
- **A host on another port.** When 47600 is taken on the host (a second copy of the game), it listens on a port the system picks, and the
  sweep does not find it. Visit it by address with the port from its log.

## Ports and firewall

| Port | Who | What |
|---|---|---|
| 47600 TCP + UDP | the host | Visitors connect here (LAN, or the internet without a relay). |
| 47610 TCP (+ UDP) | the relay | Internet play through a relay. UDP is optional, see below. |

- The first time you host, Windows Firewall may ask about pkp.exe: allow it on **private** networks. If nobody finds you, check that your
  network is set to Private (Settings, Network & internet, your connection, Network profile type) and that pkp.exe is allowed.
- **Internet without a relay:** the host forwards TCP and UDP 47600 on its router; visitors pick Visit by address with the host's public
  address and enter the host's room code.
- **Internet through a relay:** nobody forwards anything except whoever runs the relay. Any pokopia-native coop relay works. The launcher can
  run one too: **Run a relay here** on the Link Play page (while the launcher stays open), or `PokopiaLauncher --host` on a server
  (`--listen <IPv4>[:<port>]`, `--no-udp`, and pokopia-native's limits options). Forward TCP 47610 to it, and UDP 47610 too for the faster path.

With the launcher's relay and this port's build on both ends of a link, the game's packets go over UDP through the relay instead of the TCP
link, which avoids stalls on lossy connections. With pokopia-native's relay, or a pokopia-native player on the other end, they use the TCP
link as pokopia-native does. Either way they are checked end to end with the room code: the relay only forwards them. Nothing is encrypted.

## Old protocol

The launcher's Link Play page keeps the old protocol under **Advanced** (`[net] protocol = linkplay`), for a session where everyone still
has an earlier build of this port. It cannot meet pokopia-native players.

## Other ways to play local wireless (`[net] protocol`)

Pokopia's default stays `native` (pokopia-native's protocol, above), unchanged. These modes come from AnySwitch (by Chaos192,
[github.com/Chaos192/AnySwitch](https://github.com/Chaos192/AnySwitch)); they carry the game's local wireless (`ldn`) another way, chosen
in `pkp.ini` and applied the next time the game starts. Nothing in them contacts Nintendo.

- **`protocol = lan`: with Ryujinx players and consoles running ldn_mitm, on the same network.** Local wireless over the LAN in
  **ldn_mitm**'s format: the protocol of the ldn_mitm Atmosphère module and of Ryujinx's "ldn_mitm" multiplayer mode. Everyone on the
  network sees everyone's rooms; there is no relay and no room code. `lan_ip = auto` (the first up interface with a private address, or
  one of this machine's IPv4 addresses) and `lan_peers = <IPv4>[,...]` (hosts to look for when the network drops broadcasts). Ports: UDP
  and TCP 11452, the game's own UDP ports on the LAN address, and UDP 11453 for TCP between two PokoMac or AnySwitch players. Allow the
  game on private networks at the firewall prompt. The host enforces the room's player limit and the accept policy, and a player learns
  when the host left.
- **`protocol = ryuldn`: with Ryujinx players over the internet.** Rooms on a Ryujinx LDN master server ("RyuLDN"): `ryuldn_server =
  <host>[:port]` (port 30456 by default; empty: off) and `ryuldn_passphrase = Ryujinx-xxxxxxxx` for Ryujinx's private rooms. Rooms,
  scans and the game's UDP go through that server. Not yet: TCP between players, Ryujinx's direct peer-to-peer links, the accept filter.
- **`protocol = linkplay`** (the old protocol above) also gains AnySwitch's fixes: a host can host again right after a session on macOS
  (the relay's listener reuses its address), the host refuses a refused station by address and name, host socket timeouts on BSD
  sockets, TCP between local wireless nodes (vtcp: one segment resent after a timeout, then slow start; early segments wait outside the
  receive window), private networks (CreateNetworkPrivate / ConnectPrivate), and the room's status for the launcher (`net-status.json`,
  `net-control.txt` beside `pkp.log`).

Tests, all on this machine's loopback: `nxrt_services_test` (families `ldn`, `ldn_ryu`, `lanroom`, `bsd_mmsg`, `net`) and
`python tools/net/ldn_pair_test.py --exe <nxrt_services_test> --tmp <folder>` (two processes: relay, host-left, lan, crash, mitm).

## Common problems

- **The host is not found:** pick **Visit by address** and type the host's address. Check the firewall prompt and the Private network profile.
  On cafe or hotel Wi-Fi that keeps devices apart, nothing on the LAN works: use **Internet** with a relay.
- **Refused:** version or mods mismatch. Same game (2.0.1), same release or pokopia-native, same two switches.
- **Error 9005:** both players have the same player id: two copies of one save, or a player still on an earlier build (every save made
  before had the same id). Update both; a copied save still needs a new game.

## Status

Joining and walking around together work. Not yet tested: long sessions, more than two players, leaving and rejoining, play over the
internet. Expect bugs; report them with every player's `pkp.log`.
