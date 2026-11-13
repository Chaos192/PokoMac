# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""A hand-made AArch64 module for the tests of code the lift did not see (docs/ARCHITECTURE.md): mid-function entries and code
recompiled on demand. No game code: every instruction is written here. A static ELF64 (Image.from_static_elf) with a symbol table, so
function discovery sees exactly the functions it should.

    python recomp/tests/missed_fixture.py <outdir> [--level indirect]      writes fixture.elf, lifts it into <outdir>/lift (one shard)
                                                                             and writes the lift map <outdir>/fixture.map.json.gz

Layout (offsets from BASE):
  0x000 call_mid     x0 = F_mid() + 1: `adr x1, F+0x10; blr x1` (the address is taken in code, inside F)       -> 43
  0x040 F            movz x0, #1/#2/#3; ret; F+0x10: movz x0, #42; ret                                          (symbol, exact)
  0x080 call_hidden  x0 = H+0xc() where the pointer is computed (`adr x1, H; add x1, x1, #0xc`): nothing in the module says H+0xc
                     is code, so no table of the build covers it                                                -> 51
  0x0c0 H            movz x0, #0; L: add x0, x0, #1; ret; H+0xc: movz x0, #50; b L  (PokoMac: a symbol, see SYMBOLS)
                     entering at H+0xc jumps back above the entry: the unit must hold the whole function
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))

BASE = 0x5100000000
CALL_MID, F, CALL_HIDDEN, H = 0x000, 0x040, 0x080, 0x0c0


# ---- a few encodings (A64)
def movz(rd, imm):
    return 0xd2800000 | (imm << 5) | rd


def add_imm(rd, rn, imm):
    return 0x91000000 | (imm << 10) | (rn << 5) | rd


def adr(rd, pc, target):
    off = target - pc
    return 0x10000000 | ((off & 3) << 29) | (((off >> 2) & 0x7ffff) << 5) | rd


def b(pc, target):
    return 0x14000000 | (((target - pc) >> 2) & 0x3ffffff)


def blr(rn):
    return 0xd63f0000 | (rn << 5)


RET = 0xd65f03c0
PUSH = 0xa9bf7bfd           # stp x29, x30, [sp, #-16]!
POP = 0xa8c17bfd            # ldp x29, x30, [sp], #16


def text_words():
    t = {}

    def put(off, words):
        for i, w in enumerate(words):
            t[off + 4 * i] = w
    put(CALL_MID, [PUSH, adr(1, CALL_MID + 4, F + 0x10), blr(1), add_imm(0, 0, 1), POP, RET])
    put(F, [movz(0, 1), movz(0, 2), movz(0, 3), RET, movz(0, 42), RET])
    put(CALL_HIDDEN, [PUSH, adr(1, CALL_HIDDEN + 4, H), add_imm(1, 1, 0xc), blr(1), POP, RET])
    put(H, [movz(0, 0), add_imm(0, 0, 1), RET, movz(0, 50), b(H + 0x10, H + 4)])
    size = 0x100
    return b''.join(struct.pack('<I', t.get(o, 0)) for o in range(0, size, 4))


# PokoMac: H has a symbol too. AnySwitch finds it by its taken address (its funcs.py makes `adr` targets functions); PokoMac's function
# discovery does not (it stays as the build that runs Pokopia has it), so without the symbol H would be split at its branch targets.
SYMBOLS = [('call_mid', CALL_MID, 0x18), ('F', F, 0x18), ('call_hidden', CALL_HIDDEN, 0x18), ('H', H, 0x14)]


def build_elf(path):
    text = text_words()
    strtab = b'\0'
    syms = [struct.pack('<IBBHQQ', 0, 0, 0, 0, 0, 0)]
    for name, off, size in SYMBOLS:
        syms.append(struct.pack('<IBBHQQ', len(strtab), 0x12, 0, 1, BASE + off, size))   # GLOBAL FUNC in .text
        strtab += name.encode() + b'\0'
    symtab = b''.join(syms)
    shstr = b'\0.text\0.symtab\0.strtab\0.shstrtab\0'
    text_off = 0x1000
    sym_off = text_off + len(text)
    str_off = sym_off + len(symtab)
    shs_off = str_off + len(strtab)
    sh_off = (shs_off + len(shstr) + 7) & ~7
    ehdr = b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8) + struct.pack('<HHIQQQIHHHHHH', 2, 183, 1, BASE, 64, sh_off, 0, 64, 56, 1, 64, 5, 4)
    phdr = struct.pack('<IIQQQQQQ', 1, 5, text_off, BASE, BASE, len(text), len(text), 0x1000)
    sh = [bytes(64),
          struct.pack('<IIQQQQIIQQ', 1, 1, 6, BASE, text_off, len(text), 0, 0, 4, 0),
          struct.pack('<IIQQQQIIQQ', 7, 2, 0, 0, sym_off, len(symtab), 3, 1, 8, 24),
          struct.pack('<IIQQQQIIQQ', 15, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0),
          struct.pack('<IIQQQQIIQQ', 23, 3, 0, 0, shs_off, len(shstr), 0, 0, 1, 0)]
    data = bytearray(sh_off + 64 * len(sh))
    data[0:64] = ehdr
    data[64:64 + 56] = phdr
    data[text_off:text_off + len(text)] = text
    data[sym_off:sym_off + len(symtab)] = symtab
    data[str_off:str_off + len(strtab)] = strtab
    data[shs_off:shs_off + len(shstr)] = shstr
    for i, s in enumerate(sh):
        data[sh_off + 64 * i:sh_off + 64 * (i + 1)] = s
    with open(path, 'wb') as f:
        f.write(bytes(data))
    return path


def lift(outdir, level='indirect', entries=None, log=print):
    from nxrecomp import program
    os.makedirs(outdir, exist_ok=True)
    elf = build_elf(os.path.join(outdir, 'fixture.elf'))
    spec = {'kind': 'static_elf', 'path': elf, 'base': BASE, 'name': 'fixture', 'game': None, 'patches': None, 'sites': None, 'index': 0,
            'spin_yield': None, 'mid_entries': level, 'map': os.path.join(outdir, 'fixture.map.json.gz')}
    if entries:
        spec['entries'] = entries
    return program.lift_module(spec, os.path.join(outdir, 'lift'), jobs=1, shards=1, log=log)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--level', default='indirect')
    a = ap.parse_args()
    lift(a.outdir, a.level)
    return 0


if __name__ == '__main__':
    sys.exit(main())
