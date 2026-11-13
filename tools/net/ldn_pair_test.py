#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""Two-process Link Play test on one machine: two copies of the services test binary (runtime/tests/services/test_ldn_peer.cpp,
`nxrt_services_test ldnpeer`) host and visit a local wireless network through the runtime's relay and exchange UDP datagrams, broadcasts
and a TCP stream, as two players of any game would, with no game. Every socket is on this machine's loopback, the LAN discovery's too
([net] host_listen = 127.0.0.1 on both sides): nothing for a firewall to ask about, nothing reaches another machine.

    python tools/net/ldn_pair_test.py --exe build/<dir>/runtime/nxrt_services_test[.exe] --tmp <scratch folder> [--case NAME ...]

Cases (all by default):
  relay      a relay process of its own (port 40660 by default), host and visitor meet there with a room code (internet play's path)
  host-left  the same, and the host destroys the network: the visitor lands in Station with DisconnectReason DestroyedByUser
  lan        [net] server = auto: the host runs the relay inside itself (port 39660, this machine only), the visitor finds it by LAN
             discovery with no address typed. Skipped when 39660 is taken (a relay the launcher runs, a game hosting).
  crash      as lan, and the host process (its relay with it) ends without a word: the visitor lands in Station with SignalLost
  mitm       [net] protocol = lan: ldn_mitm's LAN protocol (Ryujinx's LAN mode, consoles running ldn_mitm), no relay; the host on 127.0.0.1,
             the visitor on 127.0.0.2 (Windows only: other systems have no 127.0.0.2 by default), UDP/TCP 11452
Each process gets its own temporary folder under --tmp (each test run clears its own). Exit code 0 when every case run passed.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

ROOM_CODE = '4GQ7-2M9X-K3TB-W8ZD'


def port_free(port):
    # on the loopback only: a socket on every address would make Windows Firewall ask
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        s = socket.socket(socket.AF_INET, kind)
        try:
            s.bind(('127.0.0.1', port))
        except OSError:
            return False
        finally:
            s.close()
    return True


def start(exe, tmp, name, env_add):
    d = os.path.join(tmp, name)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ)
    env.update({'TEMP': d, 'TMP': d, 'TMPDIR': d})
    env.update(env_add)
    log = open(os.path.join(d, 'out.txt'), 'w')
    p = subprocess.Popen([exe, 'ldnpeer'], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
    return p, log, os.path.join(d, 'out.txt')


def finish(procs, seconds):
    end = time.time() + seconds
    codes = []
    for p, log, path in procs:
        left = max(1, end - time.time())
        try:
            codes.append(p.wait(timeout=left))
        except subprocess.TimeoutExpired:
            p.kill()   # our own child: started above
            p.wait()
            codes.append('timeout')
        log.close()
    return codes


def tail(path, n=12):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read().splitlines()[-n:]


def run_case(exe, tmp, case, seconds, relay_port):
    common = {'NXRT_LDN_PEER_SECONDS': str(seconds), 'NXRT_LDN_PEER_PINGS': '100'}
    procs = []
    relay = None
    if case == 'mitm':
        if os.name != 'nt':
            return 'skipped (needs 127.0.0.2: Windows)', True
        if not port_free(11452):
            return 'skipped (port 11452 is taken on this machine)', True
        host_env = dict(common, NXRT_LDN_PEER='host', NXRT_LDN_PEER_PROTOCOL='lan', NXRT_LDN_PEER_LAN_IP='127.0.0.1')
        visit_env = dict(common, NXRT_LDN_PEER='visit', NXRT_LDN_PEER_PROTOCOL='lan', NXRT_LDN_PEER_LAN_IP='127.0.0.2',
                         NXRT_LDN_PEER_LAN_PEERS='127.0.0.1', NXRT_LDN_PEER_BEACONS='0')
    elif case in ('lan', 'crash'):
        if not port_free(39660):
            return 'skipped (port 39660 is taken on this machine)', True
        leave = 'crash' if case == 'crash' else 'visit'
        host_env = dict(common, NXRT_LDN_PEER='host', NXRT_LDN_PEER_SERVER='auto', NXRT_LDN_PEER_LISTEN='127.0.0.1', NXRT_LDN_PEER_LEAVE=leave)
        visit_env = dict(common, NXRT_LDN_PEER='visit', NXRT_LDN_PEER_SERVER='auto', NXRT_LDN_PEER_LISTEN='127.0.0.1', NXRT_LDN_PEER_LEAVE=leave)
    else:
        if not port_free(relay_port):
            return 'skipped (port %d is taken on this machine)' % relay_port, True
        relay = start(exe, tmp, case + '-relay', dict(NXRT_LDN_PEER='relay', NXRT_LDN_PEER_SECONDS=str(seconds + 15),
                                                      NXRT_LDN_PEER_RELAY_PORT=str(relay_port), NXRT_LDN_PEER_LISTEN='127.0.0.1'))
        time.sleep(1.0)
        server = '127.0.0.1:%d' % relay_port
        leave = 'host' if case == 'host-left' else 'visit'
        host_env = dict(common, NXRT_LDN_PEER='host', NXRT_LDN_PEER_SERVER=server, NXRT_LDN_PEER_CODE=ROOM_CODE, NXRT_LDN_PEER_LEAVE=leave)
        visit_env = dict(common, NXRT_LDN_PEER='visit', NXRT_LDN_PEER_SERVER=server, NXRT_LDN_PEER_CODE=ROOM_CODE, NXRT_LDN_PEER_LEAVE=leave)
    procs.append(start(exe, tmp, case + '-host', host_env))
    time.sleep(1.0)
    procs.append(start(exe, tmp, case + '-visit', visit_env))
    codes = finish(procs, seconds + 30)
    if relay:
        relay[0].kill()   # our own child: the relay waits out its seconds otherwise
        relay[0].wait()
        relay[1].close()
    # the crash case's host ends with _Exit(0) once its pings are answered; every other process passes with exit code 0
    ok = all(c == 0 for c in codes)
    lines = []
    for (_, _, path), who, c in zip(procs, ('host', 'visit'), codes):
        lines.append('  %s: exit %s' % (who, c))
        lines += ['    ' + x for x in tail(path, 6 if ok else 20)]
    return '\n'.join(lines), ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True, help='nxrt_services_test binary')
    ap.add_argument('--tmp', required=True, help='a scratch folder for the processes\' temporary folders and outputs')
    ap.add_argument('--case', action='append', choices=['relay', 'host-left', 'lan', 'crash', 'mitm'])
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--relay-port', type=int, default=40660)
    a = ap.parse_args()
    cases = a.case or ['relay', 'host-left', 'lan', 'crash', 'mitm']
    failed = 0
    for c in cases:
        t0 = time.time()
        text, ok = run_case(os.path.abspath(a.exe), os.path.abspath(a.tmp), c, a.seconds, a.relay_port)
        print('%s %s (%.1f s)' % ('ok    ' if ok else 'FAILED', c, time.time() - t0))
        print(text)
        failed += 0 if ok else 1
    print('%d case(s), %d failed' % (len(cases), failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
