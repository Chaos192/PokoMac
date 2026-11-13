# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""Tests of the mod manager (game-build/release_b/mods.py, docs/MODS.md) and the patch formats it uses (apply_ips.py: IPS, IPS32,
IPSwitch .pchtxt, the 0x100 + mem_off module layout). A fake title (hand-made NSOs, main.npdm, a RomFS folder; no game code or data):
every mod layout (Atmosphere, yuzu/Ryujinx, exefs_patches sets, a bare patch set, a bare RomFS tree, zip, 7z, folder), .stub deletions,
order and conflicts, the title and build checks, the effective ExeFS of code mods (patched and replaced modules) and what PokoMac's
pkp_build takes from it, what PokoMac refuses (added or removed modules, NRO plugins and patches, frame_rate.ini), pkp.ini, export,
template and pack. AnySwitch's 60 fps mod data, NRO, package (romfs.nxsrc) and frame-step hook cases are not part of PokoMac.
Run: python -I game-build/tests/test_mods.py"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'release_b'))
import apply_ips as ai  # noqa: E402
import mods  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails += 1


TID = 0x0100ABCD12340000
MAIN_BID = bytes.fromhex('aa11bb22cc33dd44') + bytes(24)
OTHER_BID = 'ee55ff6600770088'
MAIN_TEXT = b'\x00' * 0x1f0 + bytes.fromhex('08050071' '08582a1e') + bytes(8)   # subs w8, w8, #1 at 0x1f0; fmin s8, s0, s10 at 0x1f4


def nso(text, bid, ro=b'R' * 0x80, data=b'D' * 0x80):
    """A stored NSO: text at 0, ro at 0x1000, data at 0x2000 (gaps in memory, packed back to back in the file), build id at 0x40."""
    segs = [text, ro, data]
    h = bytearray(0x100)
    h[:4] = b'NSO0'
    struct.pack_into('<I', h, 0xC, 0x38)
    at = 0x100
    for i, s in enumerate(segs):
        struct.pack_into('<III', h, 0x10 + 0x10 * i, at, 0x1000 * i, len(s))
        struct.pack_into('<I', h, 0x60 + 4 * i, len(s))
        h[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = hashlib.sha256(s).digest()
        at += len(s)
    h[0x40:0x60] = bid
    return bytes(h) + b''.join(segs)


def npdm(pid):
    d = bytearray(0x200)
    d[:4] = b'META'
    struct.pack_into('<I', d, 0x70, 0x100)
    d[0x100:0x104] = b'ACI0'
    struct.pack_into('<Q', d, 0x110, pid)
    return bytes(d)


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def ips(records):
    out = bytearray(b'PATCH')
    for off, data in records:
        out += off.to_bytes(3, 'big') + len(data).to_bytes(2, 'big') + data
    return bytes(out + b'EOF')


def make_title(root):
    t = os.path.join(root, '%016X' % TID)
    put(os.path.join(t, 'exefs', 'main'), nso(MAIN_TEXT, MAIN_BID))
    put(os.path.join(t, 'exefs', 'sdk'), nso(b'S' * 0x200, bytes.fromhex('5d' * 8) + bytes(24)))
    put(os.path.join(t, 'exefs', 'rtld'), nso(b'L' * 0x100, bytes.fromhex('1d' * 8) + bytes(24)))
    put(os.path.join(t, 'exefs', 'subsdk0'), nso(b'G' * 0x200, bytes.fromhex('0d' * 8) + bytes(24)))
    put(os.path.join(t, 'exefs', 'main.npdm'), npdm(TID))
    put(os.path.join(t, 'romfs', 'a.txt'), 'game-a')
    put(os.path.join(t, 'romfs', 'sub', 'b.bin'), 'game-b')
    put(os.path.join(t, 'romfs', 'sub', 'c.bin'), 'game-c')
    put(os.path.join(t, 'romfs', 'Layout', 'menu.szs'), 'game-menu')
    put(os.path.join(t, 'pkp.ini'), '[fs]\nexefs = exefs\nromfs = romfs\nsave_dir = save\n\n[nvn]\nshader_cache = shader_cache/spirv\n')
    return t


def zip_tree(folder, out):
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _d, files in os.walk(folder):
            for n in files:
                p = os.path.join(root, n)
                z.write(p, os.path.relpath(p, folder).replace(os.sep, '/'))


def run(*argv):
    """mods.py as the app runs it: its JSON answer and exit code."""
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, '..', 'release_b', 'mods.py')] + list(argv), stdout=subprocess.PIPE)
    return json.loads(r.stdout.decode('utf-8')), r.returncode


# ---------------------------------------------------------------- patch formats
pch = '''@nsobid-AA11BB22CC33DD44000000000000000000000000000000000000000000000000
# a comment line
@flag offset_shift 0x100
@enabled
00000010 1F2003D5        // NOP (little-endian words as written)
00001004 "Hi\\n"
@disabled
00000020 DEADBEEF
@enabled
@big-endian
00002000 11223344
Credits: someone
@stop
00000030 FFFF
'''
bid, recs = ai.parse_pchtxt(pch)
check(bid == 'aa11bb22cc33dd44' + '0' * 48, 'pchtxt: @nsobid read as the build id')
check(recs == [(0x110, bytes.fromhex('1F2003D5')), (0x1104, b'Hi\n'), (0x2100, bytes.fromhex('44332211'))],
      'pchtxt: offset_shift, strings with escapes, @disabled skipped, @big-endian reverses, credits and @stop end it')
check(ai.bid_matches('aa11bb22cc33dd44', MAIN_BID.hex()) and ai.bid_matches(MAIN_BID.hex(), 'aa11bb22cc33dd44')
      and not ai.bid_matches(OTHER_BID, MAIN_BID.hex()) and not ai.bid_matches('0' * 16, '0' * 64), 'build ids match by their first 16 digits')
ips32 = b'IPS32' + (0x1104).to_bytes(4, 'big') + (2).to_bytes(2, 'big') + b'ZZ' + b'EEOF'
check(ai.parse_patch(ips32) == (True, [(0x1104, b'ZZ')]), 'IPS32 parsed')

with tempfile.TemporaryDirectory() as t:
    src = os.path.join(t, 'main')
    put(src, nso(b'\x00' * 0x200, MAIN_BID))
    p1, p2 = os.path.join(t, 'a.ips'), os.path.join(t, 'b.pchtxt')
    put(p1, ips([(0x110, b'\x11\x11\x11\x11'), (0x1100, b'ROPATCH!'), (0x50, b'HEADER')]))
    put(p2, '@nsobid-aa11bb22cc33dd44\n@flag offset_shift 0x100\n@enabled\n00000012 2222\n')
    out = os.path.join(t, 'out')
    ai.patch_nso_many(src, [p1, p2], out)
    img = open(out, 'rb').read()
    h, segs, _ = ai.decompress_nso(img)
    check(segs[0][1][0x10:0x14] == b'\x11\x11\x22\x22', 'two patches apply in order, the later wins where both write')
    check(segs[1][0] == 0x1000 and segs[1][1][:8] == b'ROPATCH!', 'a patch at 0x100 + 0x1000 lands at the start of .rodata (mem_off 0x1000)')
    check(img[0x40:0x60] == MAIN_BID and img[:4] == b'NSO0', 'a record inside the header is skipped, the header kept')
    check(struct.unpack_from('<I', img, 0x10 + 0x10)[0] == 0x1100, 'segments sit at 0x100 + mem_off in the written module')

# ---------------------------------------------------------------- layouts, install, order, conflicts, stubs
work = tempfile.mkdtemp(prefix='pokomac-mods-test-')
try:
    title_dir = make_title(work)
    title = mods.Title(title_dir)
    check(title.title_id == '%016X' % TID and title.modules['main'] == MAIN_BID.hex(), 'title: program id and module build ids')

    # 1. Atmosphere zip: a RomFS file, a code patch by build id, a patch for another version, cheats
    atm = os.path.join(work, 'src', 'atm')
    base = os.path.join(atm, 'atmosphere', 'contents', '%016x' % TID)
    put(os.path.join(base, 'romfs', 'a.txt'), 'atm-a')
    put(os.path.join(base, 'romfs', 'new', 'only.txt'), 'atm-new')
    put(os.path.join(atm, 'atmosphere', 'exefs_patches', 'FastMode', 'AA11BB22CC33DD44.ips'), ips([(0x104, b'\xAB\xCD')]))
    put(os.path.join(atm, 'atmosphere', 'exefs_patches', 'FastMode', OTHER_BID + '.ips'), ips([(0x104, b'\xEE')]))
    put(os.path.join(base, 'cheats', 'AA11BB22CC33DD44.txt'), '[Inf]\n04000000 00001000 0000270F\n')
    put(os.path.join(atm, 'readme.txt'), 'hello')
    zip_tree(atm, os.path.join(work, 'atm.zip'))
    groups = mods.detect(mods.ZipSource(os.path.join(work, 'atm.zip')).entries, title, 'atm')
    names = sorted(g['name'] for g in groups)
    check(len(groups) == 2 and 'FastMode' in names, 'atmosphere zip: the contents folder and the exefs_patches set are two mods')
    r, rc = run('install', title_dir, os.path.join(work, 'atm.zip'))
    check(rc == 0 and len(r['installed']) == 2, 'atmosphere zip installs')
    ids = [m['id'] for m in r['state']['mods']]
    fast = next(m for m in r['state']['mods'] if m['name'] == 'FastMode')
    check(any('other versions' in w for w in fast['warnings']), 'a patch for another version is named and not used')
    check(r['state']['code']['stamp'] != '' and r['state']['code_changed'], 'a code mod: the next build compiles other code')
    eff = r['state']['code']['exefs']
    em = open(os.path.join(eff, 'main'), 'rb').read()
    check(em[0x104:0x106] == b'\xAB\xCD', 'effective ExeFS: main patched at the patch offset')
    check(os.path.samefile(os.path.join(eff, 'sdk'), os.path.join(title_dir, 'exefs', 'sdk')), 'an unchanged module is a hardlink, not a copy')
    ini = mods.ini_read(os.path.join(title_dir, 'pkp.ini'))
    check(ini.get(('fs', 'romfs_mod')) == 'mods/.active/romfs_layers.txt', 'pkp.ini [fs] romfs_mod names the layer list')
    check(ini.get(('cheats', 'mod_files')) == 'mods/.active/cheats.txt', 'pkp.ini [cheats] mod_files names the cheat list')
    check(ini.get(('fs', 'exefs_mod')) is None, 'no [fs] exefs_mod before a build with the code mods finished')
    check(ini.get(('nvn', 'shader_cache')) == 'shader_cache/spirv' and ini.get(('fs', 'save_dir')) == 'save', 'other settings kept')
    cheats = open(os.path.join(title_dir, 'mods', '.active', 'cheats.txt')).read().split()
    check(len(cheats) == 1 and cheats[0].endswith('AA11BB22CC33DD44.txt'), 'the matching cheat file is listed for the engine')

    # 2. yuzu / Ryujinx folder: <tid>/<mod>/romfs, a romfs_ext .stub, an exefs with code patches; a loader that adds and removes
    #    modules, and a replaced module of another build, refused
    yz = os.path.join(work, 'src', 'yuzu', '%016X' % TID)
    put(os.path.join(yz, 'Big Textures', 'romfs', 'a.txt'), 'big-a')
    put(os.path.join(yz, 'Big Textures', 'romfs', 'sub', 'b.bin'), 'big-b')
    put(os.path.join(yz, 'No Intro', 'romfs_ext', 'sub', 'c.bin.stub'), '')
    put(os.path.join(yz, 'No Intro', 'romfs', 'Layout', 'menu.szs.stub'), '')
    put(os.path.join(yz, 'Plugin', 'exefs', 'AA11BB22CC33DD44.pchtxt'),
        '@nsobid-AA11BB22CC33DD44\n@flag offset_shift 0x100\n@enabled\n00000004 7777\n')
    r, rc = run('install', title_dir, os.path.join(work, 'src', 'yuzu'))
    got = sorted(m['name'] for m in r.get('installed', []))
    check(rc == 0 and got == ['Big Textures', 'No Intro', 'Plugin'], 'yuzu folder: three mods by their folder names')
    st = r['state']
    plugin = next(m for m in st['mods'] if m['name'] == 'Plugin')
    check(plugin['kinds'] == ['code'], 'kinds: code')
    ld = os.path.join(work, 'src', 'loader', '%016X' % TID, 'Loader')
    put(os.path.join(ld, 'exefs', 'subsdk9'), nso(b'P' * 0x200, bytes.fromhex('9d' * 8) + bytes(24)))
    put(os.path.join(ld, 'exefs', 'subsdk0.stub'), '')
    put(os.path.join(ld, 'exefs', 'main.npdm'), npdm(TID))
    rl, rc = run('install', title_dir, os.path.join(work, 'src', 'loader'))
    err = rl.get('error', '')
    check(rc == 1 and 'adds the code module subsdk9' in err and 'removes the code module subsdk0' in err and 'main.npdm' in err,
          'a mod that adds or removes modules, or brings a main.npdm, is refused with each reason')
    put(os.path.join(work, 'src', 'oldsdk', '%016X' % TID, 'Old sdk', 'exefs', 'sdk'), nso(b'Q' * 0x200, bytes.fromhex('6e' * 8) + bytes(24)))
    rl, rc = run('install', title_dir, os.path.join(work, 'src', 'oldsdk'))
    check(rc == 1 and 'another version' in rl.get('error', '') and '6e6e6e6e6e6e6e6e' in rl.get('error', ''),
          'a replaced module of another build is refused, naming its build')
    layers = open(os.path.join(title_dir, 'mods', '.active', 'romfs_layers.txt')).read().splitlines()
    body = [l for l in layers if not l.startswith('#')]
    check('delete /sub/c.bin' in body and 'delete /Layout/menu.szs' in body,
          'romfs_ext/<path>.stub and romfs/<path>.stub (where the game has <path>) become deletions')
    order = [m['id'] for m in st['mods']]
    big = next(m['id'] for m in st['mods'] if m['name'] == 'Big Textures')
    atm_id = next(m['id'] for m in st['mods'] if m['name'] != 'FastMode' and 'romfs' in m['kinds'] and m['id'] not in (big,)
                  and m['name'] not in ('No Intro', 'Plugin'))
    lay = [l.split(' ', 1)[1] for l in body if l.startswith('layer ')]
    check(lay.index('../%s/romfs' % big) > lay.index('../%s/romfs' % atm_id), 'the newer mod is above the older one (later layer wins)')
    conf = [c for c in json.load(open(os.path.join(title_dir, 'mods', '.active', 'conflicts.json'))) if c.get('path') == 'a.txt']
    check(len(conf) == 1 and conf[0]['winner'] == big, 'a.txt from two mods is a conflict, the first in the order wins')
    eff = st['code']['exefs']
    check(sorted(os.listdir(eff)) == ['main', 'main.npdm', 'rtld', 'sdk', 'subsdk0'], "effective ExeFS: the game's modules")
    em = open(os.path.join(eff, 'main'), 'rb').read()
    check(em[0x104:0x106] == b'\x77\x77', 'the newer patch wins where two mods patch the same main bytes')
    cc = [c for c in json.load(open(os.path.join(title_dir, 'mods', '.active', 'conflicts.json'))) if c.get('module') == 'main']
    check(len(cc) == 1, 'two mods patching the same bytes of main are a conflict')

    # move the older RomFS mod to the top: it wins now
    r, rc = run('move', title_dir, atm_id, '0')
    conf = [c for c in json.load(open(os.path.join(title_dir, 'mods', '.active', 'conflicts.json'))) if c.get('path') == 'a.txt']
    check(rc == 0 and r['mods'][0]['id'] == atm_id and conf[0]['winner'] == atm_id, 'move: the order decides the winner')

    # 3. what PokoMac's pkp_build takes from code.json: the effective ExeFS and its stamp (mods_code)
    import pkp_build as b
    mexefs, mstamp = b.mods_code(title_dir)
    check(mexefs == os.path.normpath(eff) and mstamp == st['code']['stamp'], 'pkp_build.mods_code: the effective ExeFS and its stamp')
    stamp_before = st['code']['stamp']

    # 4. built: [fs] exefs_mod; disabling the code mods asks for a build again and keeps exefs_mod until it is done
    r, rc = run('built', title_dir)
    ini = mods.ini_read(os.path.join(title_dir, 'pkp.ini'))
    check(rc == 0 and ini.get(('fs', 'exefs_mod')) == 'mods/.active/exefs-%s' % stamp_before and not r['code_changed'],
          'built: [fs] exefs_mod names the ExeFS the build lifted')
    fast_id = next(m['id'] for m in r['mods'] if m['name'] == 'FastMode')
    plug_id = next(m['id'] for m in r['mods'] if m['name'] == 'Plugin')
    run('disable', title_dir, fast_id)
    r, rc = run('disable', title_dir, plug_id)
    check(r['code']['stamp'] == '' and r['code_changed'], 'no code mod on: the next build compiles the game as it is')
    check(os.path.isdir(os.path.join(title_dir, 'mods', '.active', 'exefs-' + stamp_before)), 'the built ExeFS stays until the next build')
    r, rc = run('built', title_dir)
    ini = mods.ini_read(os.path.join(title_dir, 'pkp.ini'))
    check(ini.get(('fs', 'exefs_mod')) is None and not os.path.isdir(os.path.join(title_dir, 'mods', '.active', 'exefs-' + stamp_before)),
          'that build done: exefs_mod cleared, the old ExeFS removed')

    # 4b. NRO plugins and NRO patches: Pokopia loads no NRO, so PokoMac refuses them with the reason
    put(os.path.join(work, 'src', 'plug', '%016X' % TID, 'Skyline plugin', 'romfs', 'skyline', 'plugins', 'libx.nro'), bytes(0x10) + b'NRO0' + bytes(0x6C))
    r, rc = run('install', title_dir, os.path.join(work, 'src', 'plug'))
    check(rc == 1 and 'loads no NRO' in r.get('error', ''), 'an NRO plugin is refused: Pokopia loads no NRO')
    put(os.path.join(work, 'src', 'nrop', 'atmosphere', 'nro_patches', 'NroFix', '4E524F4249443031.ips'), ips([(0x100, b'\x20')]))
    r, rc = run('install', title_dir, os.path.join(work, 'src', 'nrop'))
    check(rc == 1 and 'loads no NRO' in r.get('error', ''), 'NRO patches are refused')

    # 4d. Ryujinx's containers: <mod>/romfs.bin (a RomFS image) and <mod>/exefs.nsp (a PFS0), unpacked at install, loose files on top
    def romfs_image(files):      # {path: bytes} -> a RomFS image (one level of folders is enough here)
        dirs = sorted({p.rsplit('/', 1)[0] for p in files if '/' in p})
        dmeta, fmeta, data = bytearray(), bytearray(), bytearray()
        NONE = 0xFFFFFFFF
        def dent(parent, sib, child, fil, name):
            e = struct.pack('<6I', parent, sib, child, fil, NONE, len(name)) + name
            return e + bytes(-len(e) % 4)
        def fent(parent, sib, off, size, name):
            e = struct.pack('<IIQQII', parent, sib, off, size, NONE, len(name)) + name
            return e + bytes(-len(e) % 4)
        # layout: root dir at 0, then each folder; files after
        offs, at = {'': 0}, len(dent(0, NONE, NONE, NONE, b''))
        for d in dirs:
            offs[d] = at
            at += len(dent(0, NONE, NONE, NONE, d.encode()))
        by_dir = {}
        for p in sorted(files):
            by_dir.setdefault(p.rsplit('/', 1)[0] if '/' in p else '', []).append(p)
        foffs, fat = {}, 0
        for d in [''] + dirs:
            for p in by_dir.get(d, []):
                foffs[p] = fat
                fat += len(fent(0, NONE, 0, 0, p.rsplit('/', 1)[-1].encode()))
        for d in [''] + dirs:
            sib = NONE
            if d:
                i = dirs.index(d)
                sib = offs[dirs[i + 1]] if i + 1 < len(dirs) else NONE
            child = offs[dirs[0]] if (not d and dirs) else NONE
            fl = by_dir.get(d, [])
            dmeta += dent(0, sib, child, foffs[fl[0]] if fl else NONE, d.encode())
        for d in [''] + dirs:
            fl = by_dir.get(d, [])
            for i, p in enumerate(fl):
                fmeta += fent(offs[d], foffs[fl[i + 1]] if i + 1 < len(fl) else NONE, len(data), len(files[p]), p.rsplit('/', 1)[-1].encode())
                data += files[p] + bytes(-len(files[p]) % 16)
        dmo = 0x50
        fmo = dmo + len(dmeta)
        data_off = (fmo + len(fmeta) + 15) & ~15
        head = struct.pack('<10Q', 0x50, 0, 0, dmo, len(dmeta), 0, 0, fmo, len(fmeta), data_off)
        img = head + dmeta + fmeta
        return img + bytes(data_off - len(img)) + data

    def pfs0(files):
        names = b''.join(n.encode() + b'\0' for n in files)
        ent, data = b'', b''
        no = 0
        for n, b in files.items():
            ent += struct.pack('<QQII', len(data), len(b), no, 0)
            no += len(n) + 1
            data += b
        return b'PFS0' + struct.pack('<II', len(files), len(names)) + bytes(4) + ent + names + data

    rj = os.path.join(work, 'src', 'ryuj', '%016X' % TID, 'Packed')
    put(os.path.join(rj, 'romfs.bin'), romfs_image({'sub/b.bin': b'image-b', 'top.txt': b'image-top', 'sub/c.bin': b'image-c'}))
    put(os.path.join(rj, 'romfs', 'sub', 'b.bin'), 'loose-b')
    put(os.path.join(rj, 'exefs.nsp'), pfs0({'sdk': nso(b'7' * 0x200, bytes.fromhex('5d' * 8) + bytes(24))}))
    r, rc = run('install', title_dir, os.path.join(work, 'src', 'ryuj'))
    pid = r.get('installed', [{}])[0].get('id', '')
    md = os.path.join(title_dir, 'mods', pid)
    check(rc == 0 and open(os.path.join(md, 'romfs', 'top.txt')).read() == 'image-top' and open(os.path.join(md, 'romfs', 'sub', 'c.bin')).read() == 'image-c',
          'romfs.bin unpacked into the mod\'s romfs')
    check(open(os.path.join(md, 'romfs', 'sub', 'b.bin')).read() == 'loose-b', 'a loose file wins over the container\'s (as Ryujinx serves them)')
    check(os.path.isfile(os.path.join(md, 'exefs', 'sdk'))
          and not os.path.exists(os.path.join(md, 'romfs_bin')) and not os.path.exists(os.path.join(md, 'exefs_nsp')),
          'exefs.nsp unpacked into the mod\'s exefs, the containers removed')
    check(r['state']['code']['modules'].get('sdk') == 'mod', "the effective ExeFS: sdk replaced by the mod's (the same build)")
    run('uninstall', title_dir, pid)
    run('built', title_dir)

    # 5. refusals: another game, another version
    other = os.path.join(work, 'src', 'other')
    put(os.path.join(other, '0100000000010000', 'X', 'romfs', 'a.txt'), 'x')
    zip_tree(other, os.path.join(work, 'other.zip'))
    r, rc = run('install', title_dir, os.path.join(work, 'other.zip'))
    check(rc == 1 and 'another game' in r['error'], 'a mod for another title id is refused clearly')
    ver = os.path.join(work, 'src', 'ver')
    put(os.path.join(ver, '60fps', OTHER_BID + '.pchtxt'), '@nsobid-%s\n@enabled\n0010 00\n' % OTHER_BID)
    r, rc = run('install', title_dir, ver)
    check(rc == 1 and 'another version' in r['error'] and 'aa11bb22cc33dd44' in r['error'], 'a code mod for another build is refused, '
          'naming both builds')
    put(os.path.join(work, 'src', 'junk', 'x', 'y.dat'), 'nope')
    r, rc = run('install', title_dir, os.path.join(work, 'src', 'junk'))
    check(rc == 1 and 'cannot tell' in r['error'], 'files in no known layout are refused')

    # 6. a bare patch set and a bare RomFS tree (inside a wrapper folder)
    bare = os.path.join(work, 'src', 'bare')
    put(os.path.join(bare, 'NoBloom', 'AA11BB22CC33DD44.ips'), ips([(0x108, b'\x01')]))
    r, rc = run('install', title_dir, bare)
    check(rc == 0 and r['installed'][0]['name'] == 'NoBloom' and 'code' in r['installed'][0]['kinds'], 'a bare patch set installs')
    tree = os.path.join(work, 'src', 'tree')
    put(os.path.join(tree, 'My Menu Mod', 'Layout', 'menu.szs'), 'mod-menu')
    zip_tree(tree, os.path.join(work, 'tree.zip'))
    r, rc = run('install', title_dir, os.path.join(work, 'tree.zip'))
    check(rc == 0 and r['installed'][0]['name'] == 'My Menu Mod' and 'romfs' in r['installed'][0]['kinds'],
          'a bare RomFS tree is found by the game\'s own top folders')
    menu = os.path.join(title_dir, 'mods', r['installed'][0]['id'], 'romfs', 'Layout', 'menu.szs')
    check(open(menu).read() == 'mod-menu', 'its files are placed at the game\'s paths')

    # 7. 7z archives (made by the system's bsdtar when it can write 7-Zip: each method it offers)
    tar = shutil.which('bsdtar') or shutil.which('tar')
    methods = []
    if tar:
        for m in ('store', 'deflate', 'bzip2', 'lzma1', 'lzma2'):
            out7 = os.path.join(work, 'm-%s.7z' % m)
            rr = subprocess.run([tar, '--format', '7zip', '--options', '7zip:compression=' + m, '-cf', out7, '-C', atm, '.'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if rr.returncode == 0 and os.path.isfile(out7):
                methods.append((m, out7))
    for m, out7 in methods:
        src = mods.SevenZipSource(out7)
        entries = dict(src.entries)
        dst = os.path.join(work, 'x7', m)
        want = {k: os.path.join(dst, *k.split('/')) for k in entries}
        src.extract(want)
        src.close()
        same = all(open(want[k], 'rb').read() == open(os.path.join(atm, *k.split('/')), 'rb').read() for k in entries)
        check(same and len(entries) == sum(len(f) for _r, _d, f in os.walk(atm)), '7z (%s): every file read back unchanged' % m)
    if not methods:
        print('note: no bsdtar that writes 7-Zip here: the 7z reader was not tested on this machine')
    lz = [o for m, o in methods if m.startswith('lzma')]
    if lz:
        r, rc = run('install', title_dir, lz[-1], '--name', 'From 7z')
        check(rc == 0 and len(r['installed']) == 2, 'a 7z installs like the zip')

    # 8. a user's own Settings folder becomes the lowest layer and comes back when no mod is left
    t2 = make_title(os.path.join(work, 'second'))
    mods.ini_set(os.path.join(t2, 'pkp.ini'), 'fs', 'romfs_mod', 'my-mods')
    os.makedirs(os.path.join(t2, 'my-mods'))
    r, rc = run('install', t2, os.path.join(work, 'tree.zip'))
    layers = open(os.path.join(t2, 'mods', '.active', 'romfs_layers.txt')).read().splitlines()
    check(rc == 0 and layers[1] == 'layer ' + os.path.join(t2, 'my-mods').replace('\\', '/'), 'the folder set in Settings is the lowest layer')
    r, rc = run('uninstall', t2, r['installed'][0]['id'])
    ini = mods.ini_read(os.path.join(t2, 'pkp.ini'))
    check(rc == 0 and ini.get(('fs', 'romfs_mod')) == 'my-mods' and not r['mods'], 'uninstall the last mod: the Settings folder is back')

    # 9. make: export, template, pack, and the pack installs
    r, rc = run('export', title_dir, '--out', os.path.join(work, 'export'), '--romfs', '--romfs-path', 'sub')
    ex = r['out']
    check(rc == 0 and r['romfs_files'] == 2 and sorted(os.listdir(os.path.join(ex, 'romfs', 'sub'))) == ['b.bin', 'c.bin'],
          'export: the RomFS part asked for')
    check(os.path.samefile(os.path.join(ex, 'romfs', 'sub', 'b.bin'), os.path.join(title_dir, 'romfs', 'sub', 'b.bin')),
          'export: RomFS files are hardlinks to the game\'s')
    exm = open(os.path.join(ex, 'exefs', 'main'), 'rb').read()
    check(exm[0x1100:0x1104] == b'RRRR' and json.load(open(os.path.join(ex, 'info.json')))['modules']['main']['patch_name']
          == 'aa11bb22cc33dd44', 'export: modules laid out so a file offset is the patch offset; build ids in info.json')
    r, rc = run('template', title_dir, '--out', os.path.join(work, 'make'), '--name', 'Hello Mod')
    tpl = r['out']
    put(os.path.join(tpl, 'romfs', 'a.txt'), 'hello-a')
    os.rename(os.path.join(tpl, 'exefs', 'aa11bb22cc33dd44.pchtxt.example'), os.path.join(tpl, 'exefs', 'aa11bb22cc33dd44.pchtxt'))
    with open(os.path.join(tpl, 'exefs', 'aa11bb22cc33dd44.pchtxt'), 'a') as f:
        f.write('')
    r, rc = run('pack', tpl, '--out', os.path.join(work, 'hello.zip'))
    names = zipfile.ZipFile(os.path.join(work, 'hello.zip')).namelist()
    check(rc == 0 and 'Hello Mod/romfs/a.txt' in names and 'Hello Mod/mod.json' in names and not any(n.endswith('.example') for n in names),
          'pack: the mod folder zipped, the examples left out')
    r, rc = run('install', t2, os.path.join(work, 'hello.zip'))
    check(rc == 0 and r['installed'][0]['name'] == 'Hello Mod' and r['installed'][0]['title_id'] == '%016X' % TID,
          'a packed template installs with its mod.json name')
    r, rc = run('install', title_dir, os.path.join(work, 'atm.zip'), '--store', os.path.join(work, 'store2'), '--no-ini')
    check(rc == 0 and os.path.isfile(os.path.join(work, 'store2', 'mods.json')), '--store keeps the mods in another folder')

    # frame-rate blocks (frame_rate.ini): PokoMac has no frame-step engine. A mod that is only one is refused with the reason; a
    # frame_rate.ini beside code patches is named as not used, and the rest installs
    put(os.path.join(work, 'src', 'fr', '60fps', 'frame_rate.ini'), '[frame_rate]\npresent_interval = display\nbuild = AA11BB22CC33DD44\n')
    r, rc = run('install', title_dir, os.path.join(work, 'src', 'fr'))
    check(rc == 1 and 'frame-rate engine' in r.get('error', ''), 'a frame-rate-only mod is refused: PokoMac has no frame-rate engine')
    put(os.path.join(work, 'src', 'fr2', 'Fast', 'frame_rate.ini'), '[frame_rate]\npresent_interval = display\n')
    put(os.path.join(work, 'src', 'fr2', 'Fast', 'exefs', 'AA11BB22CC33DD44.ips'), ips([(0x10C, b'\x02')]))
    r, rc = run('install', title_dir, os.path.join(work, 'src', 'fr2'))
    fm = r['installed'][0] if rc == 0 and r.get('installed') else {}
    check(rc == 0 and fm.get('kinds') == ['code'] and any('frame_rate.ini is not used' in w for w in fm.get('warnings', [])),
          'frame_rate.ini beside code patches: named as not used, the patches install')
    ini = mods.ini_read(os.path.join(title_dir, 'pkp.ini'))
    check(ini.get(('display', 'present_interval')) is None and ini.get(('mods', 'frame_rate')) is None,
          'no [display] present_interval or [mods] frame_rate written')
    run('uninstall', title_dir, fm.get('id', ''))
    run('built', title_dir)

finally:
    shutil.rmtree(work, ignore_errors=True)

print('all checks passed' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
