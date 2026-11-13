# SPDX-License-Identifier: GPL-3.0-or-later
"""python -m nxrecomp <command> ...   (see README.md)

  lift   <input> <outdir> [--game NAME] [--kind nso|nx_elf|static_elf] [--base 0x..] [--name N] [--jobs J] [--shards S]
         [--patches FILE] [--spin-yield] [--mid-entries LEVEL] [--entries FILE] [--map FILE]   lift every function of one module
  lift-at --map FILE --addr 0x.. --out DIR [--input FILE]   the unit code recompiled on demand needs for one address (ondemand.py)
  cover  <input> [--game NAME] ...   per-instruction-word translation coverage of .text
  funcs  -> python -m nxrecomp.funcs
"""
import argparse
import json
import os
import sys


def spec_from(a):
    kind = a.kind
    if not kind:
        magic = open(a.input, 'rb').read(4)
        kind = 'nso' if magic == b'NSO0' else 'static_elf'
    spec = {'kind': kind, 'path': a.input, 'base': int(a.base, 16), 'name': a.name, 'game': a.game, 'patches': a.patches, 'sites': a.sites, 'index': a.index,
            'spin_yield': getattr(a, 'spin_yield', False) or None, 'live_patch': getattr(a, 'live_patch', False) or None}
    if getattr(a, 'mid_entries', None):
        spec['mid_entries'] = a.mid_entries
    if getattr(a, 'map', None):
        spec['map'] = a.map
    if getattr(a, 'entries', None) and os.path.isfile(a.entries):     # missed.txt: this module's lines
        from .entries import load_missed
        spec['entries'] = load_missed(a.entries, {a.name}).get(a.name, [])
    return spec


def main(argv=None):
    ap = argparse.ArgumentParser(prog='nxrecomp')
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('lift', 'cover'):
        p = sub.add_parser(name)
        p.add_argument('input')
        if name == 'lift':
            p.add_argument('outdir')
            p.add_argument('--jobs', type=int, default=8)
            p.add_argument('--shards', type=int, default=0)
            p.add_argument('--spin-yield', action='store_true', help='spin-loop heads call nx_spin_hint(2) (default: the game config)')
            p.add_argument('--live-patch', action='store_true',
                           help='every function entry checks Ctx.ov (NX_OV), so the live code patches of the runtime reach direct and inlined callers (ABI 5)')
            p.add_argument('--mid-entries', choices=('pads', 'indirect', 'returns', 'leaders', 'all'),
                           help='mid-function entries (entries.py; default pads: landing pads and missed.txt)')
            p.add_argument('--entries', help="the title's missed.txt: addresses met at run time become entries")
            p.add_argument('--map', help='write the lift map (what code recompiled on demand lifts with) to this file')
        p.add_argument('--game', default='generic')
        p.add_argument('--kind', choices=['nso', 'nx_elf', 'static_elf'])
        p.add_argument('--base', default='0x7100000000')
        p.add_argument('--name', default='main')
        p.add_argument('--patches')
        p.add_argument('--index', type=int, help='module index (NxModule.index, import ids); default: position in the game config')
        p.add_argument('--sites', help='site list: patches, inline callouts, hooks (see program.load_sites)')
    p = sub.add_parser('lift-at')
    p.add_argument('--map', required=True, help='the lift map the build wrote for the module')
    p.add_argument('--addr', required=True, help='the guest address met (hex)')
    p.add_argument('--out', required=True, help='folder for od_<addr>.cpp')
    p.add_argument('--input', help='the module file (default: the one the map names)')
    a = ap.parse_args(argv)
    if a.cmd == 'lift-at':
        from . import ondemand
        r = ondemand.lift_at(a.map, int(a.addr, 16), a.out, a.input)
        print(json.dumps(r))
        return 0
    spec = spec_from(a)
    from . import program
    if a.cmd == 'lift':
        program.lift_module(spec, a.outdir, a.jobs, a.shards)
        return 0
    from .config import load_game
    img = program.open_image(spec)
    ok, bad, _ = program.cover(img, load_game(a.game))
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
