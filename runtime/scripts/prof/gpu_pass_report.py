#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""GPU time per render-pass kind from a frame trace taken with [prof] gpu_passes = true (NXRT_PROF_TRACE + NXRT_PROF_GPU_PASSES=1).

    python gpu_pass_report.py <trace.txt> [--skip S] [--seconds N] [--top K]

Each gp:<label> event is one GPU interval between two timestamps (a render pass "pass WxH cN:fmt dN", the "gap" of copies, dispatches and
barriers between passes, the "tail"); its argument is the pass's draw count. Frames = gpu.exec events in the window (one submission per
frame under [nvn] submit_batch = frame; with several submissions per frame divide by the real frame count instead). Prints ms/frame,
share of the GPU time, passes and draws per frame and us per draw for the top labels. Note: the profiler drains the pipeline at every
stamp, so absolute times run high; the shares are what to read.
"""
import argparse
import collections
import sys


def load(path):
    zones, ev = {}, []
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if line.startswith('# zone '):
                p = line.split(None, 3)
                zones[int(p[2])] = p[3].strip()
            elif line and line[0] != '#':
                p = line.split()
                if len(p) == 5:
                    ev.append((int(p[0]), int(p[1]), int(p[3]), int(p[4])))   # t0 dur thread zone arg
    return zones, ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--skip', type=float, default=70.0, help='seconds from the first event to skip (boot, menus, world load)')
    ap.add_argument('--seconds', type=float, default=60.0)
    ap.add_argument('--top', type=int, default=30)
    a = ap.parse_args()
    zones, ev = load(a.trace)
    if not ev:
        sys.exit('no events')
    t0 = min(e[0] for e in ev) + int(a.skip * 1e9)
    t1 = t0 + int(a.seconds * 1e9)
    exec_z = {z for z, n in zones.items() if n == 'gpu.exec'}
    gp = {z: n[3:] for z, n in zones.items() if n.startswith('gp:')}
    frames, exec_ns = 0, 0
    tot = collections.defaultdict(lambda: [0, 0, 0])   # label -> [ns, count, draws]
    for t, d, z, arg in ev:
        if not (t0 <= t < t1):
            continue
        if z in exec_z:
            frames += 1
            exec_ns += d
        elif z in gp:
            r = tot[gp[z]]
            r[0] += d
            r[1] += 1
            r[2] += arg
    if not frames:
        sys.exit('no gpu.exec events in the window (trace shorter than --skip?)')
    gsum = sum(r[0] for r in tot.values())
    print('window %.0f-%.0f s: %d frames, gpu.exec %.2f ms/frame, gp intervals %.2f ms/frame (%d labels)' % (
        a.skip, a.skip + a.seconds, frames, exec_ns / frames / 1e6, gsum / frames / 1e6, len(tot)))
    print('%8s %6s %7s %8s %8s  %s' % ('ms/frm', 'share', 'n/frm', 'drw/frm', 'us/draw', 'label'))
    for lab, (ns, n, dr) in sorted(tot.items(), key=lambda kv: -kv[1][0])[:a.top]:
        print('%8.3f %5.1f%% %7.1f %8.1f %8s  %s' % (ns / frames / 1e6, 100.0 * ns / max(1, gsum), n / frames, dr / frames,
                                                   ('%.1f' % (ns / dr / 1e3)) if dr else '-', lab))
    # by kind: strip the size so all passes of one format/attachment layout add up
    kind = collections.defaultdict(int)
    for lab, (ns, n, dr) in tot.items():
        kind[lab.split(' ')[0] if not lab.startswith('pass ') else 'pass ' + lab.split(' ', 2)[1]] += ns
    print('\nby kind / size:')
    for k, ns in sorted(kind.items(), key=lambda kv: -kv[1])[:15]:
        print('%8.3f %5.1f%%  %s' % (ns / frames / 1e6, 100.0 * ns / max(1, gsum), k))


if __name__ == '__main__':
    main()
