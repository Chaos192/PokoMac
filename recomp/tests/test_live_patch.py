# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""The live-patch checks of a module lifted with --live-patch (the default build since 2026-11-10, docs/CHEATS.md): NX_OV at the function's
entry and NX_OVL at each loop head (a target of a backward branch), with the function's own bit, so a patch reaches a loop that is already
running; nothing at a forward branch's target; none in a unit lifted on demand. lift_code makes a patched function's loop heads entries of
its unit (the runtime's nx_hook_resume enters it there), also when the function is too big for every instruction to be one."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from nxrecomp import ondemand, program  # noqa: E402
from nxrecomp.image import Image  # noqa: E402

FAILS = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        FAILS.append(what)


BASE = 0x7100000000
# mov w0, #0 ; L1: add w0, w0, #1 ; cmp w0, #10 ; b.eq L2 (forward) ; b L1 (backward) ; L2: ret
CODE = struct.pack('<6I', 0x52800000, 0x11000400, 0x7100281f, 0x54000040, 0x17fffffd, 0xd65f03c0)


def lift(live, ondemand_unit=False):
    img = Image('t', BASE, CODE, (0, len(CODE)))
    env = program.Env(img)
    env.full = False
    env.ends = {BASE: BASE + len(CODE)}
    env.starts = [BASE]
    env.live_patch = live
    env.ondemand = ondemand_unit
    env.module_index = 3
    return program.lift_function(env, BASE)[0]


def main():
    text = lift(True)
    check('NX_OV(c, 3u, 0u, 1u, 0x7100000000ULL);' in text, 'the entry check, with the module index and the function\'s bit')
    check('L_7100000004: NX_OVL(c, 3u, 0u, 1u, 0x7100000000ULL, 0x7100000004ULL);' in text,
          'the loop head (the backward b\'s target) checks the same bit and names its own address')
    check(text.count('NX_OVL(') == 1, 'only the loop head: the forward branch\'s target (the ret) has no check')
    plain = lift(False)
    check('NX_OV' not in plain, 'a module lifted without --live-patch has no check at all')
    unit = lift(True, ondemand_unit=True)
    check('NX_OV' not in unit, 'a unit lifted on demand has none (it is what a check sends the call to)')

    # lift_code: a patched function bigger than UNIT_ALL_MAX instructions still gets its loop heads as entries
    out = os.path.join(os.environ.get('TEMP', '/tmp'), 'nxrecomp_live_patch_test')
    # mov w0, #0 ; L: add w0, w0, #1 (UNIT_ALL_MAX + 8 times) ; cmp w0, #10 ; b.eq +2 ; b L ; ret
    words = [0x52800000] + [0x11000400] * (ondemand.UNIT_ALL_MAX + 8) + [0x7100281f, 0x54000040, 0, 0xd65f03c0]
    back_pc = len(words) - 2
    words[back_pc] = 0x14000000 | ((1 - back_pc) & 0x3ffffff)          # b L (BASE + 4)
    big = struct.pack('<%dI' % len(words), *words)
    r = ondemand.lift_code(big, BASE, BASE, out, name='patch_t', log=lambda *a: None)
    src = open(r['source'], encoding='utf-8').read()
    check('case 0x7100000004ULL' in src or 'case 0x7100000004' in src,
          'the loop head of a unit too big for every instruction to be an entry is an entry of its u_ switch')
    print('all live-patch checks passed' if not FAILS else '%d FAILED' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
