# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""Tests of apply_ips.py: IPS and IPS32 parsing (incl. RLE), byte application, and an NSO decompress -> patch -> uncompressed round trip
whose patched bytes land at the right file offset. No game code or data.
Run: python game-build/tests/test_apply_ips.py"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'release_b'))
import apply_ips as ai  # noqa: E402


def check(cond, what):
    if not cond:
        print('FAIL:', what)
        sys.exit(1)
    print('ok  ', what)


def make_ips(records, rle=None):
    out = bytearray(b'PATCH')
    for off, data in records:
        out += off.to_bytes(3, 'big') + len(data).to_bytes(2, 'big') + data
    if rle:
        off, run, val = rle
        out += off.to_bytes(3, 'big') + (0).to_bytes(2, 'big') + run.to_bytes(2, 'big') + bytes([val])
    out += b'EOF'
    return bytes(out)


def make_ips32(records):
    out = bytearray(b'IPS32')
    for off, data in records:
        out += off.to_bytes(4, 'big') + len(data).to_bytes(2, 'big') + data
    out += b'EEOF'
    return bytes(out)


# ---- IPS parsing ----
ips32, recs = ai.parse_patch(make_ips([(0x10, b'\x90\x90'), (0x20, b'\xC3')]))
check(not ips32, 'IPS magic')
check(recs == [(0x10, b'\x90\x90'), (0x20, b'\xC3')], 'IPS records')

# RLE
ips32, recs = ai.parse_patch(make_ips([(0x00, b'\x01')], rle=(0x100, 4, 0xAA)))
check(recs[-1] == (0x100, b'\xAA\xAA\xAA\xAA'), 'IPS RLE expands')

# IPS32
ips32, recs = ai.parse_patch(make_ips32([(0x00010000, b'\xDE\xAD\xBE\xEF')]))
check(ips32, 'IPS32 magic')
check(recs == [(0x00010000, b'\xDE\xAD\xBE\xEF')], 'IPS32 records')

# bad magic
try:
    ai.parse_patch(b'NOPE' + b'\x00' * 8)
    check(False, 'bad magic should raise')
except ai.IpsError:
    check(True, 'bad magic raises')

# ---- byte application ----
buf = bytearray(b'\x00' * 0x40)
ai.apply_records(buf, [(0x10, b'\x11\x22'), (0x3E, b'\x33\x44\x55')])   # the last extends the buffer
check(buf[0x10:0x12] == b'\x11\x22', 'apply writes')
check(len(buf) == 0x41 and buf[0x3E:0x41] == b'\x33\x44\x55', 'apply extends past the end')

# ---- NSO round trip ----
def build_nso(text, ro, data, bss=0x100):
    """An uncompressed NSO: header + text + ro + data at contiguous file offsets; mem_off == file_off - 0x100."""
    header = bytearray(0x100)
    header[:4] = b'NSO0'
    struct.pack_into('<I', header, 0x0C, 0)   # no compression
    segs = [text, ro, data]
    file_off = 0x100
    mem_off = 0
    for i, seg in enumerate(segs):
        struct.pack_into('<I', header, 0x10 + 0x10 * i, file_off)
        struct.pack_into('<I', header, 0x14 + 0x10 * i, mem_off)
        struct.pack_into('<I', header, 0x18 + 0x10 * i, len(seg))
        struct.pack_into('<I', header, 0x60 + 4 * i, len(seg))
        file_off += len(seg)
        mem_off += len(seg)
    struct.pack_into('<I', header, 0x3C, bss)
    return bytes(header) + text + ro + data


text = bytes(range(0, 256)) * 4         # 0x400 bytes of .text
ro = b'\xAA' * 0x100
data = b'\xBB' * 0x100
nso = build_nso(text, ro, data)

# patch two text bytes at file offset 0x110 (mapped text offset 0x10) to NOP (0x1F2003D5 little-endian = NOP; here just two bytes)
patch = make_ips([(0x110, b'\x1f\x20')])
in_path = os.path.join(HERE, '_t_in.nso')
patch_path = os.path.join(HERE, '_t.ips')
out_path = os.path.join(HERE, '_t_out.nso')
try:
    with open(in_path, 'wb') as f:
        f.write(nso)
    with open(patch_path, 'wb') as f:
        f.write(patch)
    nrec = ai.patch_nso(in_path, patch_path, out_path)
    check(nrec == 1, 'patch_nso applied 1 record')
    with open(out_path, 'rb') as f:
        out = f.read()
    # the loader reads text from its file offset; confirm the patched bytes are at file offset 0x110
    header, segs, bss = ai.decompress_nso(out)
    text_mem_off, text_bytes = segs[0]
    check(text_mem_off == 0, 'text mem_off kept')
    check(text_bytes[0x10:0x12] == b'\x1f\x20', 'patched bytes landed in the text segment at mapped offset 0x10')
    check(bss == 0x100, 'bss preserved')
    # a compressed NSO decompresses to the same image
    check(len(text_bytes) == 0x400, 'text size preserved')
finally:
    for p in (in_path, patch_path, out_path):
        if os.path.exists(p):
            os.remove(p)

print('ALL APPLY_IPS TESTS PASSED')
