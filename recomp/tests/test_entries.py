# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch). PokoMac: the fixture's H has a symbol (H is [H, H+0x14)),
# and the jit service part is left out (PokoMac has no jit service).
"""Lifter tests for code the lift did not see (entries.py, ondemand.py): which addresses become mid-function entries at each level, how
missed.txt is folded into the next lift, and what a unit recompiled on demand holds. On the hand-made module of missed_fixture.py (no
game code). Run: python recomp/tests/test_entries.py (from the repository root, Capstone installed). The run-time half (entering
through the tables, compiling and loading units) is runtime/tests/missed/missed_test.cpp."""
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import missed_fixture as fx  # noqa: E402
from nxrecomp import entries, ondemand  # noqa: E402

B = fx.BASE


def check(cond, what):
    if not cond:
        print('FAIL:', what)
        sys.exit(1)
    print('ok  ', what)


def quiet(*a):
    pass


def lifted(outdir):
    with open(os.path.join(outdir, 'lift', 'c_000.cpp'), encoding='utf-8') as f:
        text = f.read()
    with open(os.path.join(outdir, 'lift', 'module_fixture.cpp'), encoding='utf-8') as f:
        mod = f.read()
    return text, mod


def switch_of(text, fn):
    """The case addresses of u_<fn>'s entry switch (empty when the function has none)."""
    m = re.search(r'void u_%x\(Ctx \*__restrict c, uint64_t rp\) \{\n(.*?)\n  default: nx_no_entry' % fn, text, re.S)
    return sorted(int(x, 16) for x in re.findall(r'case 0x([0-9a-f]+)ULL', m.group(1))) if m else []


def main():
    tmp = tempfile.mkdtemp(prefix='nx_entries_')
    F, H, CM, CH = B + fx.F, B + fx.H, B + fx.CALL_MID, B + fx.CALL_HIDDEN

    # ---- levels
    got = {}
    for level in entries.LEVELS:
        d = os.path.join(tmp, level)
        fx.lift(d, level, log=quiet)
        got[level] = lifted(d)
    text, mod = got['pads']
    check('u_%x' % F not in text and 'entries, 0 }' in mod, 'pads: no landing pads in the module, so no function with entries (ABI 3 behaviour)')
    text, mod = got['indirect']
    check(switch_of(text, F) == [F + 0x10], 'indirect: F+0x10, whose address call_mid takes (adr), is an entry of F')
    check(switch_of(text, H) == [], 'indirect: H+0xc, only reachable by a computed pointer, is not (the case for recompiling on demand)')
    check('{ 0x%xULL, 0x%xULL, u_%x, 0u },' % (F, F + 0x18, F) in mod and 'entries, 1 }' in mod, 'indirect: the module lists F with its range')
    check('void f_%x(Ctx *__restrict c) { u_%x(c, 0); }' % (F, F) in text, 'a call of F is u_F(c, 0): its body runs from the top')
    text, _ = got['returns']
    check(switch_of(text, CM) == [CM + 0xc] and switch_of(text, CH) == [CH + 0x10], 'returns: the instruction after each blr is an entry')
    text, _ = got['leaders']
    check(switch_of(text, H) == [H + 0x4, H + 0xc], 'leaders: H\'s loop head and the instruction after its first branch (its last ends H)')
    check(F + 0x10 in switch_of(text, F), 'leaders: F+0x10 (after a ret) is one too')
    text, _ = got['all']
    check(switch_of(text, F) == [F + 4 * i for i in range(1, 6)], 'all: every instruction of F but its start')

    # ---- jump-table targets: a function whose br the jump-table pass resolved enters at each target
    class Env:
        mid_level = entries.LEVELS.index('indirect')
        jt = {0x1010: [0x1020, 0x1030, 0x2000]}
        code_refs, forced_entries = [], []

        def word(self, a):
            return 0xd503201f

    class Fn:
        has_br_fallback = False
        targets = set()
    pcs = list(range(0x1000, 0x1040, 4))
    check(entries.function_entries(Env(), Fn(), 0x1000, 0x1040, pcs, []) == [0x1020, 0x1030],
          'indirect: the targets of a resolved jump table inside the function are entries (one outside it is not)')

    # ---- the default: the build's own tables are minimal (many entries make clang's -O1 explode: entries.py)
    check(entries.DEFAULT == 'pads', 'the default level is pads: landing pads and missed.txt only')
    d = os.path.join(tmp, 'default')
    fx.lift(d, None, log=quiet)
    text, mod = lifted(d)
    check('u_%x' % F not in text and 'entries, 0 }' in mod, 'default: F+0x10 is left to recompiling on demand (no entry table row)')

    class CapEnv(Env):
        mid_level = 0
        forced_entries = list(range(0x1004, 0x1004 + 4 * 40, 4))
    check(len(entries.function_entries(CapEnv(), Fn(), 0x1000, 0x2000, list(range(0x1000, 0x2000, 4)), [0x1ff0])) == entries.MAX_FORCED + 1,
          'missed.txt gives a function at most MAX_FORCED entries (landing pads are always kept)')

    # ---- missed.txt folded into the next lift
    mt = os.path.join(tmp, 'missed.txt')
    with open(mt, 'w') as f:
        f.write('fixture\tcc\t# build x 2026-11-09 recompiled on demand\nfixture 20\nsdk 1234\n')
    check(entries.load_missed(mt, {'fixture'}) == {'fixture': [0x20, 0xcc]}, 'missed.txt: lines of this module, offsets in hex')
    d = os.path.join(tmp, 'folded')
    fx.lift(d, 'indirect', entries=entries.load_missed(mt)['fixture'], log=quiet)
    text, mod = lifted(d)
    check(switch_of(text, H) == [H + 0xc], 'an address of missed.txt inside a function becomes an entry of it (not a split)')
    check('{ 0x%xULL, f_%x },' % (B + 0x20, B + 0x20) in mod, 'one in no function (padding after call_mid) becomes a function')

    # ---- units recompiled on demand
    m = os.path.join(tmp, 'indirect', 'fixture.map.json.gz')
    r = ondemand.lift_at(m, H + 0xc, os.path.join(tmp, 'od'), log=quiet)
    src = open(r['source'], encoding='utf-8').read()
    check(r['kind'] == 'mid' and (r['start'], r['end']) == (H, H + 0x14), 'lift-at H+0xc: the unit is H again, the function holding it')
    check(switch_of(src, H) == [H + 4 * i for i in range(1, 5)], '... with every instruction an entry')
    check('static void m_%x(Ctx *__restrict c) { u_%x(c, 0x%xULL); }' % (H + 0xc, H, H + 0xc) in src, '... and m_<addr> for the address')
    check('goto L_%x;' % (H + 4) in src, '... so the jump back above the entry stays inside the unit')
    from nxrecomp import program
    env = program.make_env(ondemand.load_map(m))
    env.ondemand, env.unit = True, {H}
    check(env.call(F) == 'call_indirect(c, 0x%xULL);' % F and env.call(H) == 'f_%x(c);' % H,
          'a call out of the unit goes through the dispatcher (the unit sees no symbol of the build), one inside it stays direct')
    check('NXOD_EXPORT const NxOdUnit *nxod_unit(const NxOdApi *api)' in src and 'od_api->call_indirect(c, a)' in src,
          '... and the runtime calls go through the table the runtime hands over')
    keep, ondemand.UNIT_ALL_MAX = ondemand.UNIT_ALL_MAX, 4                  # H as if it were a big function
    r = ondemand.lift_at(m, H + 0xc, os.path.join(tmp, 'od_big'), log=quiet)
    ondemand.UNIT_ALL_MAX = keep
    src = open(r['source'], encoding='utf-8').read()
    check(not r['whole'] and switch_of(src, H) == [H + 0xc] and '0x%xULL, 0x%xULL, od_fns' % (H + 0xc, H + 0x10) in src,
          'a unit of a big function: only the address is an entry, and the unit answers for that address only')
    r = ondemand.lift_at(m, CM + 0x8, os.path.join(tmp, 'od'), log=quiet)
    src = open(r['source'], encoding='utf-8').read()
    check(r['kind'] == 'mid' and 'call_indirect(c, t)' in src, 'a unit\'s blr stays an indirect call')
    r = ondemand.lift_code(bytes.fromhex('600c80d2c0035fd6'), 0x9000, 0x9000, os.path.join(tmp, 'od'), log=quiet)
    src = open(r['source'], encoding='utf-8').read()
    check(r['kind'] == 'code' and 'c->x[0] = (uint64_t)(0x63ULL);' in src and '{ 0x9000ULL, f_9000 },' in src,
          'code made at run time (movz x0, #99; ret) lifts from its bytes and registers at its address')
    words = {0x2000 + 4 * i: w for i, w in enumerate([0xd2800c60, 0x54000040, 0xd65f03c0, 0xd2800020, 0xd65f03c0, 0xd503201f])}
    check(ondemand.code_extent(words.get, 0x2000, 0x2018) == 0x2014,
          'code made at run time ends after the ret that no earlier branch jumps past (movz; b.eq; ret; movz; ret | nop)')

    spec = ondemand.load_map(m)
    check(spec['name'] == 'fixture' and spec['mid_entries'] == 'indirect' and F + 0x10 in spec['code_refs'] and len(spec['ends']) == 4,
          'the lift map holds the inputs, the level and discovery\'s results')
    print('all checks passed')


if __name__ == '__main__':
    main()
