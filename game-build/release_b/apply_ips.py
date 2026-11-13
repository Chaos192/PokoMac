# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Portions Copyright (c) 2026 Chaos192
"""IPS / IPS32 code patches, applied to a module's decompressed image before the lift (ROADMAP phase 6, like Atmosphere's exefs_patches).

A patch is named by the module's build id: ``<build id>.ips`` (24-bit offsets, ``PATCH`` ... ``EOF``) or ``<build id>.ips32`` (32-bit
offsets, ``IPS32`` ... ``EEOF``). ``build id`` is the first 8 bytes of the NSO ModuleId as 16 lowercase hex digits (what the runtime and the
cheat engine use). pkp_build.py ``--patches <dir>`` calls patch_nso() for each module whose build id has a file, writing an uncompressed,
patched NSO that the lifter reads, so the patch becomes compiled code.

Offset convention: the patch offset is the 0x100 header plus the offset into the mapped module image (text at 0), as Atmosphere's loader
and yuzu read it (header + mapped image), and as the common exefs IPS tools (IPSwitch and the cheat sites) produce them. The NSO this tool
writes lays each segment at 0x100 + its mem_off (zero gaps between), so a file offset is that offset in every segment, ro and data too.
In a module, a record that falls in the header (offset < 0x100) is skipped and one past the image end is cut there, as Atmosphere does.

IPSwitch text patches (``.pchtxt``, read as yuzu's IPSwitchCompiler and the IPSwitch tool read them; written from the format's facts, no
yuzu code): ``@nsobid-<build id>`` names the module, ``@flag offset_shift <n>`` is added to every offset (``0x100`` for offsets into the
mapped image), ``@enabled`` / ``@disabled`` start a patch (only enabled ones apply), ``@stop`` ends the file, ``@little-endian`` /
``@big-endian`` set the byte order of hex values, ``<hex offset> <hex bytes>`` or ``<hex offset> "string"`` is a record, and ``//`` or
``#`` starts a comment. Other lines (credits after a patch) are not records.

CLI:
    python -I apply_ips.py apply  <in file>  <patch.ips|.ips32>  <out file>     # patch a raw file
    python -I apply_ips.py nso    <in.nso>   <patch>...         <out.nso>       # patch a module (decompress, patch, write uncompressed)
    python -I apply_ips.py show   <patch>                                       # print the records (.ips, .ips32, .pchtxt)
"""
import os
import struct
import sys


class IpsError(Exception):
    pass


def parse_patch(data):
    """Return (ips32, records) where records is a list of (offset, bytes). RLE records are expanded. Raises IpsError on a bad file."""
    if data[:5] == b'PATCH':
        ips32 = False
        off_size, end = 3, b'EOF'
    elif data[:5] == b'IPS32':
        ips32 = True
        off_size, end = 4, b'EEOF'
    else:
        raise IpsError('not an IPS or IPS32 patch (no PATCH / IPS32 magic)')
    i = 5
    records = []
    n = len(data)
    while True:
        if i + off_size > n:
            raise IpsError('patch truncated: no end marker')
        marker = data[i:i + off_size]
        if marker == end[:off_size] and (off_size == 4 or data[i:i + 3] == end):
            # IPS 'EOF' is 3 bytes == a 24-bit offset; IPS32 'EEOF' is 4 bytes == a 32-bit offset
            break
        offset = int.from_bytes(data[i:i + off_size], 'big')
        i += off_size
        if i + 2 > n:
            raise IpsError('patch truncated in a record header')
        size = int.from_bytes(data[i:i + 2], 'big')
        i += 2
        if size == 0:   # RLE: a 2-byte run length then one byte
            if i + 3 > n:
                raise IpsError('patch truncated in an RLE record')
            run = int.from_bytes(data[i:i + 2], 'big')
            i += 2
            value = data[i:i + 1]
            i += 1
            if run == 0:
                raise IpsError('RLE run length is 0')
            records.append((offset, value * run))
        else:
            if i + size > n:
                raise IpsError('patch truncated in a record body')
            records.append((offset, bytes(data[i:i + size])))
            i += size
    return ips32, records


def apply_records(buf, records, text_base=0):
    """Apply records to a bytearray `buf` in order. Extends the buffer if a record writes past the end (IPS allows this). Returns buf."""
    for offset, payload in records:
        off = offset + text_base
        end = off + len(payload)
        if end > len(buf):
            buf.extend(b'\x00' * (end - len(buf)))
        buf[off:end] = payload
    return buf


_ESCAPES = {'a': 7, 'b': 8, 'f': 12, 'n': 10, 'r': 13, 't': 9, 'v': 11, '\\': 92, "'": 39, '"': 34, '?': 63, '0': 0}


def _pchtxt_string(body):
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c == '\\' and i + 1 < len(body):
            out.append(_ESCAPES.get(body[i + 1], ord(body[i + 1]) & 0xFF))
            i += 2
            continue
        out += c.encode('utf-8')
        i += 1
    return bytes(out)


def _strip_comment(line):
    """The line without its comment ('/' or '#' outside quotes, as IPSwitch reads it), stripped."""
    quote = ''
    i = 0
    while i < len(line):
        c = line[i]
        if quote:
            if c == '\\':
                i += 2
                continue
            if c == quote:
                quote = ''
        elif c in '"\'':
            quote = c
        elif c in '/#':
            return line[:i].strip()
        i += 1
    return line.strip()


def parse_pchtxt(text):
    """An IPSwitch text patch: (build id as lowercase hex, '' when the file names none; records [(offset, bytes)] of its enabled patches,
    offsets with offset_shift added, in file order). Raises IpsError on a bad @flag value."""
    if isinstance(text, bytes):
        text = text.decode('utf-8', 'replace')
    bid = ''
    shift = 0
    little = True
    enabled = False         # a record before any @enabled is not applied
    records = []
    for raw in text.splitlines():
        line = _strip_comment(raw.strip().lstrip('﻿'))
        if not line:
            continue
        if line[0] == '@':
            low = line.lower()
            if low.startswith('@nsobid-'):
                bid = ''.join(ch for ch in line[8:] if ch in '0123456789abcdefABCDEF').lower()
            elif low.startswith('@stop'):
                break
            elif low.startswith('@enabled'):
                enabled = True
            elif low.startswith('@disabled'):
                enabled = False
            elif low.startswith('@flag'):
                parts = line.split()
                if len(parts) >= 3 and parts[1].lower() == 'offset_shift':
                    try:
                        shift = int(parts[2], 0)
                    except ValueError:
                        raise IpsError('bad offset_shift value: %s' % parts[2])
            elif low.startswith('@little'):
                little = True
            elif low.startswith('@big'):
                little = False
            continue
        if not enabled:
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        try:
            offset = int(parts[0], 16) + shift
        except ValueError:
            continue        # text after a patch (credits, notes) is not a record
        rest = parts[1].strip()
        if rest[:1] in ('"', "'"):
            q = rest[0]
            end = 1
            while end < len(rest) and rest[end] != q:
                end += 2 if rest[end] == '\\' else 1
            records.append((offset, _pchtxt_string(rest[1:end])))
            continue
        hexs = ''.join(rest.split())
        if not hexs or len(hexs) % 2 or any(ch not in '0123456789abcdefABCDEF' for ch in hexs):
            continue
        data = bytes.fromhex(hexs)
        records.append((offset, data if little else data[::-1]))
    return bid, records


PATCH_EXTS = ('.ips', '.ips32', '.pchtxt')


def bid_matches(named, bid):
    """Whether a patch's build id (a file name stem or @nsobid: 16 to 64 hex digits, trailing zeros optional) names module build id
    `bid` (16 or more hex digits)."""
    named, bid = named.lower().rstrip('0'), bid.lower().rstrip('0')
    if len(named) < 8:      # too short to name a module (an all-zero or empty id)
        return False
    return named[:16] == bid[:16] and (bid.startswith(named) or named.startswith(bid))


def patch_build_id(path):
    """The build id a patch file names: the stem of <bid>.ips / .ips32, the @nsobid of a .pchtxt (else its stem). '' when none."""
    name = os.path.basename(path)
    stem, ext = os.path.splitext(name)
    if ext.lower() == '.pchtxt':
        try:
            with open(path, 'rb') as f:
                bid = parse_pchtxt(f.read())[0]
        except (OSError, IpsError):
            bid = ''
        if bid:
            return bid
    stem = stem.lower()
    return stem if stem and all(c in '0123456789abcdef' for c in stem) else ''


def patches_for(patches_dir, bid):
    """Every patch file in patches_dir for module build id `bid`, in the order they apply: the folder's own files, then each subfolder's
    (Atmosphere's exefs_patches/<patch set>/), folders and files in name order."""
    out = []
    if not patches_dir or not os.path.isdir(patches_dir):
        return out
    names = sorted(os.listdir(patches_dir))
    for n in names:
        p = os.path.join(patches_dir, n)
        if os.path.isfile(p) and n.lower().endswith(PATCH_EXTS) and bid_matches(patch_build_id(p), bid):
            out.append(p)
    for n in names:
        p = os.path.join(patches_dir, n)
        if os.path.isdir(p):
            out += [q for q in (os.path.join(p, m) for m in sorted(os.listdir(p)))
                    if os.path.isfile(q) and q.lower().endswith(PATCH_EXTS) and bid_matches(patch_build_id(q), bid)]
    return out


def load_patch(path):
    """(ips32, records) of an .ips / .ips32 file, or of a .pchtxt (ips32 True: its offsets are 32-bit)."""
    with open(path, 'rb') as f:
        data = f.read()
    if path.lower().endswith('.pchtxt'):
        return True, parse_pchtxt(data)[1]
    return parse_patch(data)


# ---- NSO handling (same layout as runtime/src/loader/nso.cpp) ----

def _lz4_block_decode(src, dst_size):
    """Decode an LZ4 block stream of exactly dst_size bytes (the NSO segment format). Our own decoder, matching nso.cpp."""
    dst = bytearray(dst_size)
    ip = op = 0
    n = len(src)
    while ip < n:
        token = src[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[ip]; ip += 1; lit += b
                if b != 255:
                    break
        dst[op:op + lit] = src[ip:ip + lit]
        ip += lit; op += lit
        if ip >= n:
            break
        off = src[ip] | (src[ip + 1] << 8); ip += 2
        length = token & 15
        if length == 15:
            while True:
                b = src[ip]; ip += 1; length += b
                if b != 255:
                    break
        length += 4
        for k in range(length):
            dst[op + k] = dst[op - off + k]
        op += length
    if op != dst_size:
        raise IpsError('LZ4 stream did not expand to the expected size')
    return bytes(dst)


def decompress_nso(data):
    """Return (header: bytearray, segments: list of (mem_off, bytes), bss) from an NSO (segments decompressed)."""
    if data[:4] != b'NSO0':
        raise IpsError('not an NSO (no NSO0 magic)')
    flags = struct.unpack_from('<I', data, 0x0C)[0]
    segs = []
    for i in range(3):
        file_off = struct.unpack_from('<I', data, 0x10 + 0x10 * i)[0]
        mem_off = struct.unpack_from('<I', data, 0x14 + 0x10 * i)[0]
        dsize = struct.unpack_from('<I', data, 0x18 + 0x10 * i)[0]
        csize = struct.unpack_from('<I', data, 0x60 + 4 * i)[0]
        raw = data[file_off:file_off + (csize if (flags >> i) & 1 else dsize)]
        seg = _lz4_block_decode(raw, dsize) if (flags >> i) & 1 else bytes(raw)
        segs.append((mem_off, seg))
    bss = struct.unpack_from('<I', data, 0x3C)[0]
    return bytearray(data[:0x100]), segs, bss


def build_uncompressed_nso(header, segs):
    """Lay a decompressed NSO out: 0x100 header, then each segment at file offset 0x100 + mem_off (zero gaps between), flags cleared, so a
    patch offset (0x100 + mapped offset) is a file offset in every segment. Keeps mem_off."""
    h = bytearray(header)
    struct.pack_into('<I', h, 0x0C, 0)   # clear the compressed flags (also clears the hash-check flags; the loader checks neither)
    out = bytearray(h)
    for i, (mem_off, seg) in enumerate(segs):
        file_off = 0x100 + mem_off
        if file_off < len(out):
            raise IpsError('NSO segments overlap in memory')
        out += bytes(file_off - len(out))
        struct.pack_into('<I', h, 0x10 + 0x10 * i, file_off)
        struct.pack_into('<I', h, 0x14 + 0x10 * i, mem_off)
        struct.pack_into('<I', h, 0x18 + 0x10 * i, len(seg))
        struct.pack_into('<I', h, 0x60 + 4 * i, len(seg))   # csize == dsize (uncompressed)
        out += seg
    out[:0x100] = h
    return out


def apply_to_image(buf, records, text_base=0):
    """Apply records to an uncompressed NSO laid out by build_uncompressed_nso. A record's bytes in the header (< 0x100) are skipped and a
    record is cut at the image end (a module does not grow), as Atmosphere's loader applies them. Returns the number of bytes written."""
    n = 0
    for offset, payload in records:
        off = offset + text_base
        if off < 0x100:
            payload = payload[0x100 - off:]
            off = 0x100
        end = min(off + len(payload), len(buf))
        if end <= off:
            continue
        buf[off:end] = payload[:end - off]
        n += end - off
    return n


def patch_nso_many(in_path, patch_paths, out_path, text_base=0):
    """Read an NSO, apply every patch of patch_paths in order (.ips, .ips32, .pchtxt; where two write the same bytes the later wins) to the
    decompressed NSO and write it uncompressed. Returns the number of records applied."""
    with open(in_path, 'rb') as f:
        data = f.read()
    header, segs, _bss = decompress_nso(data)
    image = build_uncompressed_nso(header, segs)   # the patches target this layout (header + segments at 0x100 + mem_off)
    count = 0
    for p in patch_paths:
        _ips32, records = load_patch(p)
        apply_to_image(image, records, text_base)
        count += len(records)
    with open(out_path, 'wb') as f:
        f.write(image)
    return count


def patch_nso(in_path, patch_path, out_path, text_base=0):
    """patch_nso_many with one patch file. Returns the number of records applied."""
    return patch_nso_many(in_path, [patch_path], out_path, text_base)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[0]
    if cmd == 'apply' and len(argv) == 4:
        with open(argv[1], 'rb') as f:
            buf = bytearray(f.read())
        _ips32, records = load_patch(argv[2])
        apply_records(buf, records)
        with open(argv[3], 'wb') as f:
            f.write(buf)
        print('applied %d records' % len(records))
        return 0
    if cmd == 'nso' and len(argv) >= 4:
        n = patch_nso_many(argv[1], argv[2:-1], argv[-1])
        print('patched NSO: %d records, wrote %s' % (n, argv[-1]))
        return 0
    if cmd == 'show' and len(argv) == 2:
        ips32, records = load_patch(argv[1])
        print('%s, %d records' % ('IPS32' if ips32 else 'IPS', len(records)))
        for off, payload in records:
            print('  0x%08x: %s' % (off, payload.hex()))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
