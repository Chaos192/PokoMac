// SPDX-License-Identifier: GPL-3.0-or-later
//! This PC's public IPv4 address, for an internet host to give its visitors (#245). The launcher's Link Play page only listed
//! the PC's own adapters (LAN and VPN addresses), so a host had nothing to share but the 127.0.0.1 in its Relay address field,
//! which on a visitor's PC means the visitor's own PC. Asked only while the page shows an internet host; one small HTTPS GET to
//! a what-is-my-IP service, the answer kept for the rest of the launcher's run.

use std::io::Read;
use std::net::Ipv4Addr;
use std::sync::Mutex;
use std::time::Duration;

/// Plain-text services that answer with the caller's address, tried in order.
const SERVICES: [&str; 2] = ["https://api.ipify.org", "https://ipv4.icanhazip.com"];
const TIMEOUT: Duration = Duration::from_secs(5);

static FOUND: Mutex<Option<String>> = Mutex::new(None);

/// The public address, or None when no service answered with one. A failure is not kept: the next call asks again.
pub fn lookup() -> Option<String> {
    if let Some(ip) = FOUND.lock().unwrap().clone() {
        return Some(ip);
    }
    let agent = ureq::AgentBuilder::new()
        .timeout(TIMEOUT)
        .user_agent(&format!("PokoMac/{}", env!("CARGO_PKG_VERSION")))
        .build();
    for url in SERVICES {
        let Ok(resp) = agent.get(url).call() else { continue };
        let mut body = String::new();
        if resp.into_reader().take(64).read_to_string(&mut body).is_err() {
            continue;
        }
        if let Some(ip) = parse(&body) {
            let ip = ip.to_string();
            *FOUND.lock().unwrap() = Some(ip.clone());
            return Some(ip);
        }
    }
    None
}

/// A service's answer, if it is an address visitors on the internet could reach: not private, loopback, link-local, carrier-grade
/// NAT (100.64/10), documentation, multicast or reserved.
pub fn parse(body: &str) -> Option<Ipv4Addr> {
    let ip: Ipv4Addr = body.trim().parse().ok()?;
    let [a, b, ..] = ip.octets();
    let shared = a == 100 && (64..128).contains(&b);
    let bad = ip.is_private() || ip.is_loopback() || ip.is_link_local() || ip.is_unspecified() || ip.is_broadcast()
        || ip.is_documentation() || ip.is_multicast() || shared || a == 0 || a >= 240;
    (!bad).then_some(ip)
}

#[cfg(test)]
mod tests {
    use super::parse;

    #[test]
    fn public_addresses_pass() {
        assert_eq!(parse("8.8.8.8\n").map(|i| i.to_string()).as_deref(), Some("8.8.8.8"));
        assert_eq!(parse("  1.2.3.4  ").map(|i| i.to_string()).as_deref(), Some("1.2.3.4"));
    }

    #[test]
    fn what_visitors_cannot_reach_is_refused() {
        for s in ["127.0.0.1", "192.168.0.5", "10.0.0.1", "172.16.4.4", "169.254.1.1", "100.64.0.1", "100.127.255.255", "203.0.113.5",
                  "224.0.0.1", "0.0.0.0", "255.255.255.255", "240.0.0.1"] {
            assert_eq!(parse(s), None, "{s}");
        }
    }

    #[test]
    fn junk_is_refused() {
        for s in ["", "<html>", "2001:db8::1", "1.2.3", "1.2.3.4.5"] {
            assert_eq!(parse(s), None, "{s}");
        }
    }
}
