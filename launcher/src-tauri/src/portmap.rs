// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
//! Opens the Link Play port on the router while this Mac hosts over the internet, so visitors get in without the player forwarding the
//! port by hand: UPnP IGD first (igd-next), then NAT-PMP (RFC 6886) on the default gateway (`route -n get default` on macOS).
//!
//! The ports. A game that hosts over the internet without a relay (pkp.ini [net] internet = true, mode auto or auto:<port>, no relay)
//! listens on TCP and UDP 47600, or the port of auto:<port> (runtime third_party/pokopia-coop coop.h COOP_PORT_DEFAULT, coop_mpnet.c
//! cmp_host_open: the TCP control links and the UDP datagrams share one port; through a relay nothing listens on this Mac). This Mac's
//! relay listens on TCP and UDP 47610 (relay/mod.rs DEFAULT_PORT and its UDP extension). Both protocols are mapped, each on the same
//! external port as the local one, because visitors are told that port and the game tells them its UDP port as it is.
//!
//! A mapping is asked for on a thread of its own with short timeouts, with a two-hour lease renewed every hour while the game (or the
//! relay) runs, and removed when it ends. Nothing waits for it: Play starts at once, and the Link Play page shows how it went (`report`).
//! The page's switch is launcher.json `port_forward_off` (forwarding is on by default).

use serde::Serialize;
use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

pub const LEASE_SECS: u32 = 7200;
const RENEW: Duration = Duration::from_secs(3600);
/// How long the router has to answer the UPnP search.
const SEARCH: Duration = Duration::from_secs(3);
const NATPMP_PORT: u16 = 5351;
const DESCRIPTION: &str = "PokoMac Link Play";
pub const GAME_PORT: u16 = 47600;
/// What the messages call this computer.
const HERE: &str = if cfg!(target_os = "macos") { "this Mac" } else { "this PC" };

/// One mapping as the Link Play page shows it.
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct MapStatus {
    #[serde(skip)]
    id: u64,
    /// "game" or "relay"
    pub purpose: String,
    pub port: u16,
    /// "trying", "open" or "failed"
    pub state: String,
    /// "UPnP" or "NAT-PMP", once open
    pub method: String,
    /// the router's internet address with the port, once open and known
    pub external: String,
    /// this Mac's address on the local network, which the router forwards to
    pub local: String,
    /// what happened, in plain words
    pub message: String,
    /// open, but the router's own internet address is private or shared (another router in front, or carrier-grade NAT)
    pub unreachable: bool,
}

#[derive(Serialize, Debug)]
pub struct Report {
    /// the player has not switched automatic forwarding off
    pub enabled: bool,
    pub maps: Vec<MapStatus>,
}

static STATUS: Mutex<Vec<MapStatus>> = Mutex::new(Vec::new());
static NEXT_ID: AtomicU64 = AtomicU64::new(1);
/// Raised by `cancel_all` (the player switched forwarding off): every lease started before it removes its mapping and ends.
static GENERATION: AtomicU64 = AtomicU64::new(0);

fn put(s: MapStatus) {
    let mut all = STATUS.lock().unwrap_or_else(|e| e.into_inner());
    all.retain(|x| x.id != s.id);
    all.push(s);
}

fn clear(id: u64) {
    STATUS.lock().unwrap_or_else(|e| e.into_inner()).retain(|x| x.id != id);
}

pub fn report(cfg: Option<&Path>) -> Report {
    Report { enabled: cfg.is_none_or(enabled), maps: STATUS.lock().unwrap_or_else(|e| e.into_inner()).clone() }
}

/// launcher.json says forwarding is on (it is unless the player switched it off). Read as JSON: `--play` has no window.
pub fn enabled(cfg: &Path) -> bool {
    let v: serde_json::Value = std::fs::read_to_string(cfg).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
    !v.get("port_forward_off").and_then(|x| x.as_bool()).unwrap_or(false)
}

/// Every running lease removes its mapping and ends (the switch was turned off).
pub fn cancel_all() {
    GENERATION.fetch_add(1, Ordering::SeqCst);
}

/// The port the game will host on over the internet with nothing in between, from pkp.ini [net] as the runtime reads it (CONFIG.md):
/// protocol native, internet on, no relay, mode auto (a missing key) or auto:<port>. None: it visits, plays on the LAN, hosts through a
/// relay, or Link Play is off.
pub fn game_host_port(game_dir: &Path) -> Option<u16> {
    let bytes = std::fs::read(game_dir.join(crate::settings::PKP_INI)).ok()?;
    let ini = crate::ini::Ini::parse(&String::from_utf8_lossy(&bytes));
    host_port_of(&|k: &str| ini.get("net", k).map(|v| v.trim().to_ascii_lowercase()))
}

fn host_port_of(get: &dyn Fn(&str) -> Option<String>) -> Option<u16> {
    let on = |v: Option<String>| v.is_some_and(|v| matches!(v.as_str(), "1" | "true" | "yes" | "on"));
    if !matches!(get("protocol").as_deref(), None | Some("") | Some("native")) || !on(get("internet")) || !get("relay").unwrap_or_default().is_empty() {
        return None;
    }
    match get("mode").as_deref() {
        None | Some("auto") => Some(GAME_PORT),
        Some(m) => m.strip_prefix("auto:").and_then(|p| p.parse::<u16>().ok()).filter(|p| *p != 0),
    }
}

/// The game's mapping for one run (play.rs start and run_cli), when forwarding is on and pkp.ini hosts over the internet.
pub fn for_game(game_dir: &Path, cfg: Option<&Path>) -> Option<Lease> {
    if !cfg.is_none_or(enabled) {
        return None;
    }
    game_host_port(game_dir).map(|p| Lease::start("game", p))
}

// ---- the lease ----------------------------------------------------------------------------------------------------------------------

/// A mapping kept open on a thread of its own until `finish`.
pub struct Lease {
    port: u16,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Lease {
    pub fn start(purpose: &str, port: u16) -> Lease {
        let id = NEXT_ID.fetch_add(1, Ordering::SeqCst);
        let gen = GENERATION.load(Ordering::SeqCst);
        let stop = Arc::new(AtomicBool::new(false));
        let base = MapStatus {
            id,
            purpose: purpose.to_string(),
            port,
            state: "trying".into(),
            method: String::new(),
            external: String::new(),
            local: String::new(),
            message: format!("Asking your router to open port {}...", port),
            unreachable: false,
        };
        put(base.clone());
        let s = stop.clone();
        let thread = std::thread::spawn(move || {
            let ended = || s.load(Ordering::SeqCst) || GENERATION.load(Ordering::SeqCst) != gen;
            keep_open(base, &ended);
            clear(id);
        });
        Lease { port, stop, thread: Some(thread) }
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    /// Ends the lease; its thread removes the mapping. Waits up to `wait` for that (`--play` ends the process right after).
    pub fn finish(mut self, wait: Duration) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(t) = self.thread.take() {
            let until = Instant::now() + wait;
            while !t.is_finished() && Instant::now() < until {
                std::thread::sleep(Duration::from_millis(50));
            }
            if t.is_finished() {
                let _ = t.join();
            }
        }
    }
}

/// Sleeps `total` in short steps; true as soon as `ended` says so.
fn sleep_unless(ended: &dyn Fn() -> bool, total: Duration) -> bool {
    let start = Instant::now();
    while start.elapsed() < total {
        if ended() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    ended()
}

fn keep_open(base: MapStatus, ended: &dyn Fn() -> bool) {
    let port = base.port;
    let opened = match open(port) {
        Ok(o) => o,
        Err(why) => {
            if !ended() {
                put(MapStatus { state: "failed".into(), message: why, ..base });
            }
            while !sleep_unless(ended, Duration::from_secs(3600)) {}
            return;
        }
    };
    if ended() {
        opened.mapped.close(port);
        return;
    }
    let external = opened.external.map(|ip| format!("{}:{}", ip, port)).unwrap_or_default();
    let unreachable = opened.external.is_some_and(|ip| crate::public_ip::parse(&ip.to_string()).is_none());
    let message = match (unreachable, opened.external) {
        (true, Some(ip)) => format!(
            "Your router opened port {} ({}), but its own internet address is {}, a private one: there is another router in front of it, or your internet provider shares one address between customers. Visitors cannot reach you this way. Use a relay instead.",
            port, opened.method, ip
        ),
        (_, Some(_)) => format!("Router port opened automatically ({}). Visitors reach {} at {}.", opened.method, HERE, external),
        _ => format!("Router port opened automatically ({}).", opened.method),
    };
    put(MapStatus { state: "open".into(), method: opened.method.into(), external, local: opened.local.to_string(), message, unreachable, ..base });
    let every = opened.renew_every.min(RENEW);
    while !sleep_unless(ended, every) {
        if let Err(e) = opened.mapped.renew(port) {
            eprintln!("Link Play port {}: renewing the router's mapping failed: {}", port, e);
        }
    }
    opened.mapped.close(port);
}

// ---- asking the router ------------------------------------------------------------------------------------------------------------------

enum Mapped {
    Upnp { gw: Box<igd_next::Gateway>, local: SocketAddr, lease: u32 },
    NatPmp { gateway: Ipv4Addr },
}

struct Opened {
    mapped: Mapped,
    external: Option<Ipv4Addr>,
    local: Ipv4Addr,
    method: &'static str,
    renew_every: Duration,
}

impl Mapped {
    fn renew(&self, port: u16) -> Result<(), String> {
        match self {
            Mapped::Upnp { gw, local, lease } => {
                for proto in [igd_next::PortMappingProtocol::TCP, igd_next::PortMappingProtocol::UDP] {
                    gw.add_port(proto, port, *local, *lease, DESCRIPTION).map_err(|e| e.to_string())?;
                }
                Ok(())
            }
            Mapped::NatPmp { gateway } => {
                for op in [OP_TCP, OP_UDP] {
                    natpmp_map(*gateway, op, port, port, LEASE_SECS).map_err(|e| e.to_string())?;
                }
                Ok(())
            }
        }
    }

    fn close(&self, port: u16) {
        match self {
            Mapped::Upnp { gw, .. } => {
                for proto in [igd_next::PortMappingProtocol::TCP, igd_next::PortMappingProtocol::UDP] {
                    let _ = gw.remove_port(proto, port);
                }
            }
            Mapped::NatPmp { gateway } => {
                // a delete is a request with lifetime 0 and external port 0 (RFC 6886 3.4)
                for op in [OP_TCP, OP_UDP] {
                    let _ = natpmp_map(*gateway, op, port, 0, 0);
                }
            }
        }
    }
}

/// This Mac's address on the way to `to` (the route the system would take; nothing is sent).
fn local_ip_towards(to: IpAddr) -> Option<IpAddr> {
    let s = UdpSocket::bind(("0.0.0.0", 0)).ok()?;
    s.connect((to, 9)).ok()?;
    s.local_addr().ok().map(|a| a.ip())
}

enum Fail {
    /// nothing answered
    Silent,
    /// the router answered and said no
    Refused(String),
    /// the router forwards this port to another device already
    InUse,
}

/// UPnP, then NAT-PMP. Err: the sentence the Link Play page shows.
fn open(port: u16) -> Result<Opened, String> {
    let upnp = match upnp_open(port) {
        Ok(o) => return Ok(o),
        Err(f) => f,
    };
    let natpmp = match natpmp_open(port) {
        Ok(o) => return Ok(o),
        Err(f) => f,
    };
    let here = default_gateway().map(IpAddr::V4).and_then(local_ip_towards).map(|ip| format!(" ({})", ip)).unwrap_or_default();
    Err(match (upnp, natpmp) {
        (Fail::InUse, _) | (_, Fail::InUse) => {
            format!("Your router already forwards port {} to another device, so it could not open it for {}. Free the port in the router's settings, or use a relay.", port, HERE)
        }
        (Fail::Refused(why), _) | (_, Fail::Refused(why)) => format!(
            "Your router refused to open port {} by itself ({}). Forward port {} (TCP and UDP) to {}{} in the router's settings, or use a relay.",
            port, why, port, HERE, here
        ),
        _ => format!(
            "Your router did not answer a request to open port {}: UPnP is probably off on it. Turn on UPnP in the router's settings, or forward port {} (TCP and UDP) to {}{} by hand, or use a relay.",
            port, port, HERE, here
        ),
    })
}

fn upnp_open(port: u16) -> Result<Opened, Fail> {
    use igd_next::{AddPortError, PortMappingProtocol};
    let mut opts = igd_next::SearchOptions::default();
    opts.timeout = Some(SEARCH);
    opts.single_search_timeout = Some(SEARCH);
    let gw = igd_next::search_gateway(opts).map_err(|_| Fail::Silent)?;
    let Some(IpAddr::V4(here)) = local_ip_towards(gw.addr.ip()) else { return Err(Fail::Refused("no IPv4 route to the router".into())) };
    let local = SocketAddr::V4(SocketAddrV4::new(here, port));
    let mut lease = LEASE_SECS;
    for (i, proto) in [PortMappingProtocol::TCP, PortMappingProtocol::UDP].into_iter().enumerate() {
        let mut r = gw.add_port(proto, port, local, lease, DESCRIPTION);
        // some routers only take mappings without an end: those are removed when the game ends all the same
        if matches!(r, Err(AddPortError::OnlyPermanentLeasesSupported)) {
            lease = 0;
            r = gw.add_port(proto, port, local, lease, DESCRIPTION);
        }
        if let Err(e) = r {
            if i > 0 {
                let _ = gw.remove_port(PortMappingProtocol::TCP, port);
            }
            return Err(match e {
                AddPortError::PortInUse => Fail::InUse,
                AddPortError::ActionNotAuthorized => Fail::Refused("not allowed".into()),
                AddPortError::RequestError(_) => Fail::Silent,
                other => Fail::Refused(other.to_string()),
            });
        }
    }
    let external = match gw.get_external_ip() {
        Ok(IpAddr::V4(ip)) => Some(ip),
        _ => None,
    };
    Ok(Opened { mapped: Mapped::Upnp { gw: Box::new(gw), local, lease }, external, local: here, method: "UPnP", renew_every: RENEW })
}

// ---- NAT-PMP (RFC 6886) -----------------------------------------------------------------------------------------------------------------

const OP_UDP: u8 = 1;
const OP_TCP: u8 = 2;

#[derive(Debug, PartialEq)]
enum NatError {
    NoAnswer,
    /// result code: 2 not authorized or refused, 3 network failure, 4 out of resources
    Code(u16),
    Bad,
}

impl std::fmt::Display for NatError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            NatError::NoAnswer => write!(f, "no answer"),
            NatError::Code(2) => write!(f, "not allowed"),
            NatError::Code(3) => write!(f, "the router is not connected to the internet"),
            NatError::Code(4) => write!(f, "the router has no room for another mapping"),
            NatError::Code(c) => write!(f, "result code {}", c),
            NatError::Bad => write!(f, "an answer that does not make sense"),
        }
    }
}

/// Sends `req` to the gateway and waits for the answer to opcode `op`: 4 tries, 250 ms then twice as long each time (the RFC's
/// schedule, cut short: about 4 s in all).
fn natpmp_ask(gateway: Ipv4Addr, req: &[u8], op: u8) -> Result<Vec<u8>, NatError> {
    let sock = UdpSocket::bind(("0.0.0.0", 0)).map_err(|_| NatError::NoAnswer)?;
    sock.connect((gateway, NATPMP_PORT)).map_err(|_| NatError::NoAnswer)?;
    let mut wait = Duration::from_millis(250);
    let mut buf = [0u8; 64];
    for _ in 0..4 {
        if sock.send(req).is_err() {
            return Err(NatError::NoAnswer);
        }
        let until = Instant::now() + wait;
        while let Some(left) = until.checked_duration_since(Instant::now()).filter(|d| !d.is_zero()) {
            let _ = sock.set_read_timeout(Some(left));
            match sock.recv(&mut buf) {
                Ok(n) if n >= 4 && buf[0] == 0 && buf[1] == 128 + op => return Ok(buf[..n].to_vec()),
                Ok(_) => continue,
                Err(_) => break,
            }
        }
        wait *= 2;
    }
    Err(NatError::NoAnswer)
}

fn map_request(op: u8, internal: u16, external: u16, lifetime: u32) -> [u8; 12] {
    let mut r = [0u8; 12];
    r[1] = op;
    r[4..6].copy_from_slice(&internal.to_be_bytes());
    r[6..8].copy_from_slice(&external.to_be_bytes());
    r[8..12].copy_from_slice(&lifetime.to_be_bytes());
    r
}

/// A mapping answer: (the external port, the lifetime granted).
fn parse_map_reply(op: u8, b: &[u8]) -> Result<(u16, u32), NatError> {
    if b.len() < 16 || b[0] != 0 || b[1] != 128 + op {
        return Err(NatError::Bad);
    }
    let code = u16::from_be_bytes([b[2], b[3]]);
    if code != 0 {
        return Err(NatError::Code(code));
    }
    Ok((u16::from_be_bytes([b[10], b[11]]), u32::from_be_bytes([b[12], b[13], b[14], b[15]])))
}

fn parse_address_reply(b: &[u8]) -> Option<Ipv4Addr> {
    (b.len() >= 12 && b[0] == 0 && b[1] == 128 && b[2] == 0 && b[3] == 0).then(|| Ipv4Addr::new(b[8], b[9], b[10], b[11]))
}

fn natpmp_map(gateway: Ipv4Addr, op: u8, internal: u16, external: u16, lifetime: u32) -> Result<(u16, u32), NatError> {
    parse_map_reply(op, &natpmp_ask(gateway, &map_request(op, internal, external, lifetime), op)?)
}

fn natpmp_open(port: u16) -> Result<Opened, Fail> {
    let gateway = default_gateway().ok_or(Fail::Silent)?;
    let mut granted = LEASE_SECS;
    for (i, op) in [OP_TCP, OP_UDP].into_iter().enumerate() {
        let undo = || {
            if i > 0 {
                let _ = natpmp_map(gateway, OP_TCP, port, 0, 0);
            }
        };
        match natpmp_map(gateway, op, port, port, LEASE_SECS) {
            Ok((ext, life)) if ext == port => granted = granted.min(life.max(120)),
            Ok((_, _)) => {
                // another port outside: visitors would be told the wrong one, so the mapping is not kept
                let _ = natpmp_map(gateway, op, port, 0, 0);
                undo();
                return Err(Fail::InUse);
            }
            Err(NatError::NoAnswer) => {
                undo();
                return Err(Fail::Silent);
            }
            Err(e) => {
                undo();
                return Err(Fail::Refused(e.to_string()));
            }
        }
    }
    let external = natpmp_ask(gateway, &[0, 0], 0).ok().and_then(|b| parse_address_reply(&b));
    let local = match local_ip_towards(IpAddr::V4(gateway)) {
        Some(IpAddr::V4(ip)) => ip,
        _ => Ipv4Addr::UNSPECIFIED,
    };
    Ok(Opened { mapped: Mapped::NatPmp { gateway }, external, local, method: "NAT-PMP", renew_every: Duration::from_secs(granted as u64 / 2) })
}

/// The default gateway: macOS `route -n get default`, Linux /proc/net/route. None on Windows (UPnP only there).
fn default_gateway() -> Option<Ipv4Addr> {
    #[cfg(target_os = "macos")]
    {
        let out = crate::host::command("/sbin/route").args(["-n", "get", "default"]).stderr(std::process::Stdio::null()).output().ok()?;
        gateway_of_route(&String::from_utf8_lossy(&out.stdout))
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        gateway_of_proc(&std::fs::read_to_string("/proc/net/route").ok()?)
    }
    #[cfg(windows)]
    {
        None
    }
}

/// `route -n get default` (macOS): the line `gateway: 192.168.1.1`.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn gateway_of_route(text: &str) -> Option<Ipv4Addr> {
    text.lines().find_map(|l| l.trim().strip_prefix("gateway:")).and_then(|g| g.trim().parse().ok())
}

/// /proc/net/route (Linux): the gateway of the first default route, little-endian hex.
#[cfg_attr(any(target_os = "macos", windows), allow(dead_code))]
fn gateway_of_proc(text: &str) -> Option<Ipv4Addr> {
    text.lines().skip(1).find_map(|l| {
        let f: Vec<&str> = l.split_whitespace().collect();
        (f.len() >= 3 && f[1] == "00000000").then(|| u32::from_str_radix(f[2], 16).ok()).flatten().filter(|g| *g != 0).map(|g| Ipv4Addr::from(g.swap_bytes()))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn port_for(pairs: &[(&str, &str)]) -> Option<u16> {
        let get = |k: &str| pairs.iter().find(|(key, _)| *key == k).map(|(_, v)| v.to_string());
        host_port_of(&get)
    }

    #[test]
    fn only_an_internet_host_without_a_relay_is_forwarded() {
        assert_eq!(port_for(&[("internet", "true")]), Some(47600), "a missing mode is auto");
        assert_eq!(port_for(&[("internet", "true"), ("mode", "auto"), ("relay", "")]), Some(47600));
        assert_eq!(port_for(&[("internet", "1"), ("mode", "auto:50000"), ("protocol", "native")]), Some(50000));
        assert_eq!(port_for(&[("internet", "false"), ("mode", "auto")]), None, "same network");
        assert_eq!(port_for(&[("internet", "true"), ("relay", "203.0.113.5:47610")]), None, "through a relay nothing listens here");
        assert_eq!(port_for(&[("internet", "true"), ("mode", "join:203.0.113.5")]), None, "a visitor");
        assert_eq!(port_for(&[("internet", "true"), ("mode", "off")]), None);
        assert_eq!(port_for(&[("internet", "true"), ("mode", "")]), None, "empty is off");
        assert_eq!(port_for(&[("internet", "true"), ("protocol", "linkplay")]), None);
        assert_eq!(port_for(&[("internet", "true"), ("mode", "auto:0")]), None);
    }

    #[test]
    fn the_gateway_is_read_from_the_route_table() {
        let mac = "   route to: default\ndestination: default\n       mask: default\n    gateway: 192.168.1.1\n  interface: en0\n";
        assert_eq!(gateway_of_route(mac), Some(Ipv4Addr::new(192, 168, 1, 1)));
        assert_eq!(gateway_of_route("route: writing to routing socket: not in table\n"), None);
        let linux = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\nwlan0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\nwlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\n";
        assert_eq!(gateway_of_proc(linux), Some(Ipv4Addr::new(192, 168, 1, 1)));
    }

    #[test]
    fn nat_pmp_messages() {
        assert_eq!(map_request(OP_TCP, 47600, 47600, 7200), [0, 2, 0, 0, 0xB9, 0xF0, 0xB9, 0xF0, 0, 0, 0x1C, 0x20]);
        let ok = [0, 130, 0, 0, 0, 0, 0, 9, 0xB9, 0xF0, 0xB9, 0xF0, 0, 0, 0x0E, 0x10];
        assert_eq!(parse_map_reply(OP_TCP, &ok), Ok((47600, 3600)));
        assert_eq!(parse_map_reply(OP_UDP, &ok), Err(NatError::Bad), "the answer to another request");
        let refused = [0, 129, 0, 2, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(parse_map_reply(OP_UDP, &refused), Err(NatError::Code(2)));
        assert_eq!(parse_address_reply(&[0, 128, 0, 0, 0, 0, 0, 1, 203, 0, 113, 5]), Some(Ipv4Addr::new(203, 0, 113, 5)));
        assert_eq!(parse_address_reply(&[0, 128, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0]), None);
    }

    #[test]
    fn the_switch_is_on_unless_turned_off() {
        let dir = std::env::temp_dir().join(format!("pkp-portmap-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let cfg = dir.join("launcher.json");
        assert!(enabled(&cfg), "no file");
        std::fs::write(&cfg, r#"{"port_forward_off": false}"#).unwrap();
        assert!(enabled(&cfg));
        std::fs::write(&cfg, r#"{"port_forward_off": true}"#).unwrap();
        assert!(!enabled(&cfg));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
