# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""First-run game build of release model (b): turns the player's own dump into pkp.exe on their PC.

    python -I tools/build/pkp_build.py --exefs <dump>/updates/<v>/exefs --game <install folder> [--jobs N] [--keep]
    python -I tools/build/pkp_build.py --check --game <install folder>

The package ships no game code. This script
  1. verifies the dump's ExeFS against dump_hashes.json (the supported build: sha256 of every module + main's build ID),
  2. lifts the four NSOs with the bundled lifter (recomp/nxrecomp) into <game>/build/lift,
  3. compiles the generated C++ with the bundled Zig (zig c++, mingw target) into <game>/build/obj, in parallel,
  4. links pkp.exe = sdk/pkp_stub.cpp + the lifted objects + sdk/pkp_rt.lib (the runtime, pkp_rt.dll in the package),
  5. deletes the generated sources and objects (unless --keep) and records the result in <game>/build/done.json.

The build stamp (state.json, done.json) is the dump (main build ID, module hashes) plus tool_stamp(): everything in this
package that goes into pkp.exe (lifter sources, main.sites, compile flags, the SDK files with pkp_rt.lib). A release that
changes any of them makes an older stamp stale: Set up then lifts, compiles and links again from scratch (the old pkp.exe
stays until the new one is linked over it). --check compares <game>/build/done.json with this package's tools only (no
dump check, well under a second) and prints one JSON line {"current": bool, "reason": text}; exit 0 current, 3 not.

Object cache: every compiled object is also kept in <game>/build/objcache under a key of its source text, the generated headers,
the lifter's include/ tree, the compiler and the flags (class ObjCache), so Set up again after a release that changes only some
translation units compiles only those. It survives the stamp reset and the cleanup after linking, is trimmed to --cache-mb
(default 3072, 0 = off) and never serves a partial or damaged entry.

Live code patches (the cheats overlay; ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch): every module is lifted with
--live-patch by default (each function entry and loop head checks Ctx.ov: recomp ctx.h NX_OV / NX_OVL), so a patch the overlay makes reaches
every caller while the game runs. The lift also writes a lift map per module into <game>/build/missed/map and the manifest
<game>/build/missed/manifest.tsv: with them the runtime (core/missed.cpp) makes a patched function again on demand, with this script's
--ondemand (the bundled lifter and Zig), cached in build/missed/cache. --no-live-patch lifts without the checks (to measure their cost).

Resumable: a module's lift counts once its marker file is written, an object once it is renamed into place, so a stopped
build continues where it stopped. A file named <game>/build/cancel stops it (the launcher's Pause).
Output for the launcher, one per line on stdout:  @progress <phase> <done> <total> <text>  /  @error <text>
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, FIRST_COMPLETED, wait

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.join(HERE, 'recomp')
SDK = os.path.join(HERE, 'sdk')
# Windows: the bundled Zig. Linux (the native launcher): a Linux build of the same Zig version, which it fetches into
# tools/linux/zig (PKP_ZIG overrides it); it cross-compiles the same x86_64-windows-gnu objects.
# macOS: the launcher fetches the macOS build of the same Zig into tools/macos/zig; the game is a native arm64 Mach-O program `pkp`
# linked against libpkp_rt.dylib (the runtime built for macOS), which sits next to it.
MACOS = sys.platform == 'darwin'
ZIG = (os.path.join(HERE, 'zig', 'zig.exe') if os.name == 'nt'
       else os.environ.get('PKP_ZIG') or os.path.join(os.path.dirname(HERE), 'macos' if MACOS else 'linux', 'zig', 'zig'))
EXE = 'pkp' if MACOS else 'pkp.exe'
RT_LIB = 'libpkp_rt.dylib' if MACOS else 'pkp_rt.lib'     # in sdk/: what the link resolves the runtime's exports against
TARGET = ['-target', 'aarch64-macos', '-mcpu=apple_m1'] if MACOS else ['-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16']
FORMAT = 2          # bump when the build layout or the stamp changes: older builds (partial or done) are then started over
APP_ID = 'io.github.chaos192.pokomac'   # the launcher's identifier (tauri.conf.json); pkp's identity on macOS starts with it (bundle_id)
IDENTITY = 1        # macOS: how pkp gets its identity (write_identity), in the build stamp: a change links pkp again
# the lifter as game-build/scripts/make-release.ps1 ships it: paths under RECOMP, the same in the zip (tools/build/recomp/...)
# and in the repo (recomp/...)
LIFTER = ['nxrecomp', 'include/nxrecomp', 'games/pkp/CMakeLists.txt', 'games/pkp/config.py']
SITES = 'games/pkp/main.sites'
# what the link takes from sdk/ besides the lifted objects and pkp_rt.lib (stamped on its own, as 'runtime')
SDK_FILES = ['pkp_stub.cpp', 'pkp_rt_api.h'] + ([] if MACOS else ['pkp.rc', 'pkp.manifest'])

# name, NSO file, guest base, NxModule.index, translation units (as recomp/games/pkp/CMakeLists.txt)
MODULES = [
    ('main', 'main', 0x7100000000, 0, 230),
    ('sdk', 'sdk', 0x7500000000, 1, 45),
    ('subsdk0', 'subsdk0', 0x7600000000, 2, 4),
    ('rtld', 'rtld', 0x7700000000, 3, 1),
]
# recomp/cmake/NxRecomp.cmake flags for the lifted code, in clang (not clang-cl) syntax
CXXFLAGS = TARGET + ['-std=c++20', '-O1', '-w', '-fno-exceptions', '-fno-rtti',
                     '-fno-strict-aliasing', '-ffp-contract=off', '-fno-threadsafe-statics']
COMPILER = [ZIG, 'c++']                 # the compile command before the flags (tests put a fake one here)
CACHE_MB = 3072                         # default size cap of <game>/build/objcache
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def progress(phase, done, total, text):
    say('progress', phase, done, total, text)


class Fail(Exception):
    pass


class Cancelled(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def tree_hash(root, rels):
    """One sha256 over the files under root/rel for each rel (a file or a folder, walked in sorted order): each file's path
    relative to root and its sha256. Python's bytecode caches are left out (the lifter's own runs write them); a missing
    path counts as missing, so the hash still compares."""
    h = hashlib.sha256()
    for rel in rels:
        top = os.path.join(root, *rel.split('/'))
        if os.path.isfile(top):
            files = [rel]
        elif os.path.isdir(top):
            files = []
            for d, dirs, names in os.walk(top):
                dirs[:] = [x for x in dirs if x != '__pycache__']
                files += [os.path.relpath(os.path.join(d, n), root).replace('\\', '/') for n in names if not n.endswith('.pyc')]
        else:
            h.update(('%s\0missing\0' % rel).encode())
            continue
        for f in sorted(files):
            h.update(('%s\0%s\0' % (f, sha256(os.path.join(root, *f.split('/'))))).encode())
    return h.hexdigest()


def tool_stamp(live_patch=True):
    """The part of the build stamp this package decides: a release that changes any of it rebuilds pkp.exe. live_patch: every function
    entry and loop head checks for live code patches (NX_OV, NX_OVL)."""
    stamp = {'lifter': tree_hash(RECOMP, LIFTER), 'sites': tree_hash(RECOMP, [SITES]), 'sdk': tree_hash(SDK, SDK_FILES),
             'runtime': sha256(os.path.join(SDK, RT_LIB)), 'cxxflags': CXXFLAGS, 'modules': [list(m) for m in MODULES],
             'live_patch': bool(live_patch)}
    if MACOS:
        stamp['identity'] = IDENTITY
    return stamp


def mods_code(game):
    """The game's code mods (docs/MODS.md, mods.py; ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch): the effective
    ExeFS the mod manager made (<game>/mods/.active/code.json: patched or replaced modules, the game's own for the rest) and its stamp,
    or (None, '') when no code mod is on. The build lifts that ExeFS instead of the dump's, once the dump itself is verified."""
    code = read_json(os.path.join(game, 'mods', '.active', 'code.json'))
    if not isinstance(code, dict) or not code.get('stamp') or not code.get('exefs'):
        return None, ''
    exefs = code['exefs'] if os.path.isabs(code['exefs']) else os.path.join(game, code['exefs'])
    return os.path.normpath(exefs), str(code['stamp'])


def mods_built(game):
    """After a finished build: [fs] exefs_mod in pkp.ini names the ExeFS it lifted, or nothing without code mods (mods.py built), so the
    runtime maps exactly the modules that were compiled. A game folder with no mods folder is left as it is."""
    if not os.path.isdir(os.path.join(game, 'mods')):
        return
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import mods
    title = mods.Title(game)
    mods.built(title, mods.Store(title))


def check(game):
    """Whether pkp.exe and <game>/build/done.json were built with this package's tools: (current, reason)."""
    if not os.path.isfile(os.path.join(game, EXE)):
        return False, '%s is not built yet' % EXE
    done = read_json(os.path.join(game, 'build', 'done.json'))
    if not isinstance(done, dict):
        return False, '%s has no finished build record' % EXE
    if done.get('format') != FORMAT or not isinstance(done.get('tools'), dict):
        return False, '%s was built by an older release' % EXE
    want = tool_stamp()
    changed = [k for k in want if done['tools'].get(k) != want[k]]
    if changed:
        return False, 'this release changes ' + ', '.join(changed)
    if done.get('mods', '') != mods_code(game)[1]:
        return False, 'the code mods changed (Mods)'
    return True, '%s matches this release' % EXE


def read_json(path):
    """The JSON in path, or None if it is missing or unreadable (a file cut short by a crash or power loss starts over)."""
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json(path, value):
    """Write through a temporary file and rename, so a stop mid-write never leaves half a file behind (flushed first: flush())."""
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(value, f, indent=1)
    flush_replace(tmp, path)


# Power cuts (from Recompia, Dittobyte/Recompia a3b1f351): every file a later run trusts without reading it again (a lifted module's
# sources before its marker, an object and a cache entry before their rename, pkp before done.json) is flushed to the disk first. A
# power cut can otherwise keep a rename and a file's size but not its data: an object read as zeros past that point failed the link on
# every Set up again. Zig's own cache lasts one run, and an object whose end reads as zeros is compiled again (whole_object()).
def flush(path):
    """Write path's data to the disk (FlushFileBuffers on Windows, fsync elsewhere). NTFS journals a rename and a file's size
    at once, its data later: after a power cut a file renamed a moment before can have its size and read as zeros."""
    with open(path, 'r+b') as f:
        os.fsync(f.fileno())


def flush_replace(tmp, path):
    """os.replace(tmp, path) once tmp's data is on the disk, so path is never there without it."""
    flush(tmp)
    os.replace(tmp, path)


def whole_object(path):
    """Whether the object file path is there and does not end in 4 KiB of zero bytes. An object ends with its symbol and string
    tables, never zeros; a power cut that kept the size but not the data leaves zeros there."""
    try:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            f.seek(max(0, size - 4096))
            tail = f.read()
    except OSError:
        return False
    return size > 0 and tail.count(0) != len(tail)


def nso_short(path):
    """How many bytes an NSO lacks against the segments its header lists (0 = complete; a text-mode FTP copy drops a byte per CR LF pair).
    From pokopia-native (MIT), nxs2.py nso_short @7c3655d."""
    with open(path, 'rb') as f:
        h = f.read(0x70)
    if len(h) < 0x70 or h[:4] != b'NSO0':
        return 0
    flags = struct.unpack_from('<I', h, 0xC)[0]
    end = max(struct.unpack_from('<I', h, 0x10 + 0x10 * i)[0] + struct.unpack_from('<I', h, 0x60 + 4 * i if flags & (1 << i) else 0x18 + 0x10 * i)[0]
              for i in range(3))
    return max(0, end - os.path.getsize(path))


# main build ID (first 16 bytes) of the older game version; their sources disagree on its number, so player text says "an older version"
OLDER_MAIN_BUILD = '816c5358e8178d802fcec4a7ce5a741a'


def verify(exefs):
    want = json.load(open(os.path.join(HERE, 'dump_hashes.json'), encoding='utf-8'))
    missing = [n for n in want['sha256'] if not os.path.isfile(os.path.join(exefs, n))]
    if missing:
        raise Fail('The game files folder %s is missing %s. Pick the exefs of a complete dump.' % (exefs, ', '.join(missing)))
    with open(os.path.join(exefs, 'main'), 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        raise Fail('exefs/main is not a decrypted NSO. Extract the game files with your own keys first (decrypted exefs and romfs).')
    bid = head[0x40:0x54].hex()
    if bid != want['main_build_id'] and bid.startswith(OLDER_MAIN_BUILD):
        raise Fail('These game files are from an older version. Update the game on your Switch 2 to %s, then dump again.' % want['version'].split()[0])
    if bid != want['main_build_id']:
        raise Fail('This is a different version of the game (main build ID %s). This release supports %s (build ID %s).'
                   % (bid, want['version'], want['main_build_id']))
    names = list(want['sha256'])
    for n in names:
        short = nso_short(os.path.join(exefs, n))
        if short:
            raise Fail('exefs/%s is %d bytes shorter than its own header says: the dump was copied in text mode, which damages it. '
                       'Copy the exefs folder again in binary mode (FileZilla: Transfer > Transfer type > Binary).' % (n, short))
    for i, n in enumerate(names):
        progress('verify', i, len(names), 'Checking your game files: %s' % n)
        got = sha256(os.path.join(exefs, n))
        if got != want['sha256'][n]:
            raise Fail('exefs/%s does not match the supported %s dump (sha256 %s). Re-dump the game, or check that the update %s is installed.'
                       % (n, want['version'], got, want['version']))
    progress('verify', len(names), len(names), 'Game files match %s' % want['version'])
    return want


def popen(cmd, **kw):
    # creation flags are Windows-only; on Linux the launcher lowered this script's priority, which its children inherit
    flags = CREATE_NO_WINDOW | BELOW_NORMAL if os.name == 'nt' else 0
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            creationflags=flags, **kw)


# The lifter (recomp/nxrecomp) is a program of its own, recomp/lifter_cli.py (tools/build/recomp/lifter_cli.py in a release): this script
# runs it and reads its answers, it never imports the lifter. Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch, bc2aacdd).
def lifter_cmd(recomp, *args):
    """The command line of the lifter run from the folder `recomp` (lifter_cli.py <recomp> <command> ...)."""
    cli = os.path.join(os.path.abspath(recomp), 'lifter_cli.py')
    if not os.path.isfile(cli):
        raise Fail("The build tools are incomplete: %s (the lifter's command line) is missing. Download the release again so its tools "
                   'are complete.' % cli)
    return [sys.executable, '-IB', cli, os.path.abspath(recomp)] + list(args)


def lifter_call(recomp, request, stem, on_line=None):
    """One call of the lifter (lifter_cli.py call): the request as JSON in <stem>.request.json, each line the lifter logs to on_line, ->
    its answer (<stem>.result.json). A call that fails raises Fail with its exit code and its last lines."""
    req, res = stem + '.request.json', stem + '.result.json'
    for f in (req, res):
        if os.path.exists(f):
            os.remove(f)
    with open(req, 'w', encoding='utf-8') as f:
        json.dump(dict(request, result=res), f)
    try:
        p = subprocess.Popen(lifter_cmd(recomp, 'call', req), stdout=subprocess.PIPE, stdin=subprocess.DEVNULL,
                             creationflags=CREATE_NO_WINDOW if os.name == 'nt' else 0)
        tail = []
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
            tail.append(line)
            del tail[:-30]
            if on_line:
                on_line(line)
        rc = p.wait()
        r = read_json(res) if rc == 0 else None
        if not isinstance(r, dict):
            raise Fail('The lifter (%s) failed (exit %d):\n%s' % (request.get('op'), rc, '\n'.join(tail)))
        return r
    finally:
        for f in (req, res):
            if os.path.exists(f):
                os.remove(f)


class Runner:
    """Runs tool processes; kills them all when the cancel file appears."""

    def __init__(self, cancel_file):
        self.cancel_file = cancel_file
        self.procs = set()
        self.lock = threading.Lock()
        self.cancelled = False
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        while True:
            if os.path.exists(self.cancel_file):
                self.cancelled = True
                with self.lock:
                    for p in list(self.procs):
                        try:
                            p.kill()
                        except OSError:
                            pass
            time.sleep(0.3)

    def check(self):
        if self.cancelled or os.path.exists(self.cancel_file):
            self.cancelled = True
            raise Cancelled()

    def run(self, cmd, env=None, cwd=None, on_line=None):
        self.check()
        p = popen(cmd, env=env, cwd=cwd)
        with self.lock:
            self.procs.add(p)
        tail = []
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip()
            tail.append(line)
            del tail[:-30]
            if on_line:
                on_line(line)
        rc = p.wait()
        with self.lock:
            self.procs.discard(p)
        self.check()
        return rc, '\n'.join(tail)


def lift(runner, exefs, gen, jobs, missed_dir=None, missed_txt=None, live_patch=True):
    """One lifter process per module (the lifter itself runs `jobs` worker processes). missed_dir: where the lift maps go (code made again
    on demand is lifted with them); missed_txt: the game's missed.txt, whose addresses become entries or functions of this lift; live_patch:
    the NX_OV / NX_OVL checks (the cheats overlay's live code patches)."""
    sites = os.path.join(RECOMP, 'games', 'pkp', 'main.sites')
    for k, (name, nso, base, index, shards) in enumerate(MODULES):
        out = os.path.join(gen, name)
        marker = os.path.join(out, '.lifted')
        if os.path.isfile(marker):
            continue
        progress('lift', k, len(MODULES), 'Translating the game code: %s (%d of %d)' % (name, k + 1, len(MODULES)))
        if os.path.isdir(out):
            shutil.rmtree(out)
        os.makedirs(out)
        args = ['lift', os.path.join(exefs, nso), out, '--name', name, '--base', hex(base), '--shards', str(shards),
                '--jobs', str(jobs), '--game', 'pkp', '--index', str(index)]
        if name == 'main':
            args += ['--sites', sites]
        if missed_dir:
            os.makedirs(os.path.join(missed_dir, 'map'), exist_ok=True)
            args += ['--map', os.path.join(missed_dir, 'map', '%s.json.gz' % name)]
        if missed_txt and os.path.isfile(missed_txt):
            args += ['--entries', missed_txt]
        if live_patch:
            args += ['--live-patch']
        cmd = lifter_cmd(RECOMP, 'nxrecomp', *args)    # one lifter process (it runs its own pool), `python -m nxrecomp lift ...`
        rc, tail = runner.run(cmd, cwd=RECOMP)
        if rc != 0:
            raise Fail('Translating %s failed (exit %d):\n%s' % (name, rc, tail))
        for d, _, files in os.walk(out):     # the marker makes the sources trusted (flush())
            for n in files:
                flush(os.path.join(d, n))
        open(marker, 'w').write('ok\n')
    progress('lift', len(MODULES), len(MODULES), 'Game code translated')


def free_memory():
    """Available physical memory in bytes (Windows, Linux), or None."""
    if os.name != 'nt':
        try:
            with open('/proc/meminfo') as f:
                for line in f:
                    if line.startswith('MemAvailable:'):
                        return int(line.split()[1]) * 1024
        except (OSError, ValueError, IndexError):
            pass
        return None
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [(n, ctypes.c_uint64) for n in
                        ('total', 'avail', 'pagetotal', 'pageavail', 'virttotal', 'virtavail', 'extavail')]
        m = MS(len=ctypes.sizeof(MS))
        return m.avail if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else None
    except (AttributeError, OSError):
        return None


class ObjCache:
    """Compiled objects kept by content, so a lifter or main.sites change recompiles only the translation units whose text changed.

    An entry is <key>-<size>.o in the cache folder. key = sha256 of the unit's source bytes, the generated headers next to it,
    the lifter's include/ tree, the compiler (its file hash) and the flags. The compiler or the flags changing gives other keys,
    so nothing stale is ever hit. An entry is written to a .tmp file, flushed and renamed, and a hit must have exactly the size in
    its name and be whole_object(), so a cut-off write or a damaged file is a miss (and is deleted). Hits are copied out, never linked. The folder is
    trimmed to the size cap, least recently used first (a hit refreshes the entry's time). Any cache error is a miss."""

    def __init__(self, folder, cap_bytes, compiler_id, flags_text, include_hash):
        self.folder = folder
        self.cap = cap_bytes
        self.base = '\0'.join([compiler_id, flags_text, include_hash])
        self.headers = {}                 # folder of the sources -> hash of the .h files in it
        self.lock = threading.Lock()
        self.hits = self.misses = 0
        os.makedirs(folder, exist_ok=True)

    def _headers(self, d):
        with self.lock:
            if d not in self.headers:
                h = hashlib.sha256()
                for n in sorted(os.listdir(d)):
                    if n.endswith(('.h', '.hpp', '.inc')):
                        h.update(('%s\0%s\0' % (n, sha256(os.path.join(d, n)))).encode())
                self.headers[d] = h.hexdigest()
            return self.headers[d]

    def key(self, src):
        h = hashlib.sha256()
        h.update(self.base.encode())
        h.update(b'\0' + self._headers(os.path.dirname(src)).encode() + b'\0')
        h.update(sha256(src).encode())
        return h.hexdigest()

    def _find(self, key):
        try:
            for n in os.listdir(self.folder):
                if n.startswith(key + '-') and n.endswith('.o'):
                    return n
        except OSError:
            pass
        return None

    def fetch(self, key, dst):
        """Copy the cached object for key to dst (through dst + '.tmp'); False on a miss or a bad entry."""
        n = self._find(key)
        if n:
            src = os.path.join(self.folder, n)
            tmp = dst + '.tmp'
            try:
                want = int(n[len(key) + 1:-2])
                if want > 0 and os.path.getsize(src) == want and whole_object(src):
                    shutil.copyfile(src, tmp)
                    if os.path.getsize(tmp) == want:
                        flush_replace(tmp, dst)
                        os.utime(src)
                        self.hits += 1
                        return True
                    os.remove(tmp)
                else:
                    os.remove(src)        # not the size it says, or its end is zeros: damaged, never used
            except (OSError, ValueError):
                pass
        self.misses += 1
        return False

    def store(self, key, obj):
        try:
            size = os.path.getsize(obj)
            if size <= 0:
                return
            final = os.path.join(self.folder, '%s-%d.o' % (key, size))
            tmp = final + '.%d.tmp' % threading.get_ident()
            shutil.copyfile(obj, tmp)
            if os.path.getsize(tmp) != size:
                os.remove(tmp)
                return
            flush_replace(tmp, final)
        except OSError:
            pass

    def trim(self):
        """Delete leftover .tmp files, then the least recently used entries until the folder fits the cap."""
        try:
            items = []
            for n in os.listdir(self.folder):
                p = os.path.join(self.folder, n)
                if n.endswith('.tmp'):
                    os.remove(p)
                elif n.endswith('.o'):
                    st = os.stat(p)
                    items.append((st.st_mtime, st.st_size, p))
            total = sum(i[1] for i in items)
            for _, size, p in sorted(items):
                if total <= self.cap:
                    break
                os.remove(p)
                total -= size
        except OSError:
            pass


def make_cache(folder, cap_mb):
    """The object cache for this package's compiler and flags, or None when cap_mb is 0 or the compiler cannot be hashed."""
    if cap_mb <= 0:
        return None
    try:
        compiler = ','.join(sha256(c) if os.path.isfile(c) else c for c in COMPILER)
        return ObjCache(folder, cap_mb << 20, compiler, ' '.join(CXXFLAGS), tree_hash(RECOMP, ['include']))
    except OSError:
        return None


def compile_all(runner, gen, obj, jobs, env, cache=None):
    units = []
    for name, _, _, _, shards in MODULES:
        d = os.path.join(gen, name)
        for f in sorted(os.listdir(d)):
            if f.endswith('.cpp'):
                units.append((name, os.path.join(d, f)))
    total = len(units)
    todo = []
    for name, src in units:
        o = os.path.join(obj, name, os.path.basename(src)[:-4] + '.o')
        if not whole_object(o):
            todo.append((name, src, o))
    done = total - len(todo)
    for name, _, _, _, _ in MODULES:
        os.makedirs(os.path.join(obj, name), exist_ok=True)
    keys = {}
    if cache:
        rest = []
        for t in todo:
            keys[t[1]] = cache.key(t[1])
            if cache.fetch(keys[t[1]], t[2]):
                done += 1
            else:
                rest.append(t)
        todo = rest
    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
    # biggest first, so the long ones do not end up last on a single core
    todo.sort(key=lambda t: -os.path.getsize(t[1]))

    def one(t):
        name, src, o = t
        tmp = o + '.tmp'
        cmd = COMPILER + CXXFLAGS + ['-I', os.path.join(RECOMP, 'include'), '-I', os.path.dirname(src), '-c', src, '-o', tmp]
        rc, tail = runner.run(cmd, env=env)
        if rc != 0:
            return 'Compiling %s/%s failed (exit %d):\n%s' % (name, os.path.basename(src), rc, tail)
        flush_replace(tmp, o)
        if cache:
            cache.store(keys[src], o)
        return None

    # a big unit takes a GB or two in clang; with too many at once memory runs out and clang dies on every unit with
    # exit 541541187 (0x20474343: an uncaught C++ exception, std::bad_alloc)
    # ponytail: 2 GiB per job is a guess from the unit sizes; measure if machines with plenty of RAM get capped
    avail = free_memory()
    if avail:
        jobs = max(1, min(jobs, avail // (2 << 30)))
    failed = []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        pending = {ex.submit(one, t): t for t in todo}
        try:
            while pending:
                fin, _ = wait(pending, return_when=FIRST_COMPLETED)
                for f in fin:
                    t = pending.pop(f)
                    if f.result():
                        failed.append(t)
                        continue
                    done += 1
                    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
        except BaseException:
            for f in pending:
                f.cancel()
            raise
    for t in failed:                     # again one at a time with all the memory: only a failure here is real
        err = one(t)
        if err:
            raise Fail(err)
        done += 1
        progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
    if cache:
        cache.trim()


def clear_high_entropy_va(exe):
    """pkp.exe keeps host allocations low (the guest ranges at 0x71_0000_0000 must stay free): no high-entropy ASLR,
    as /HIGHENTROPYVA:NO does for the clang-cl build. zig's linker has no flag for it, so clear the PE bit."""
    with open(exe, 'r+b') as f:
        f.seek(0x3c)
        pe = int.from_bytes(f.read(4), 'little')
        f.seek(pe + 24)
        if f.read(2) != b'\x0b\x02':
            raise Fail('pkp.exe is not a PE32+ image')
        off = pe + 24 + 70
        f.seek(off)
        dc = int.from_bytes(f.read(2), 'little') & ~0x20
        f.seek(off)
        f.write(dc.to_bytes(2, 'little'))


def macho_set_sdk_version(path, major):
    """macOS: writes SDK version `major`.0 (and the same minimum OS) into the executable's LC_BUILD_VERSION. The kernel lays out a process
    whose executable says SDK 12 the classic way, without the randomly slid 23 GiB data range it gives newer programs, which would land in
    the guest's address window (runtime include/nxrt/memory.h). zig's linker has no -platform_version, so the header is patched and the
    image signed again (ad hoc) afterwards."""
    LC_BUILD_VERSION, LC_VERSION_MIN_MACOSX = 0x32, 0x24
    ver = major << 16
    with open(path, 'r+b') as f:
        hdr = f.read(32)
        if struct.unpack_from('<I', hdr, 0)[0] != 0xFEEDFACF:
            raise Fail('%s is not a 64-bit Mach-O image' % path)
        ncmds = struct.unpack_from('<I', hdr, 16)[0]
        off, done = 32, False
        for _ in range(ncmds):
            f.seek(off)
            cmd, size = struct.unpack('<II', f.read(8))
            if cmd == LC_BUILD_VERSION:          # platform, minos, sdk
                f.seek(off + 12)
                f.write(struct.pack('<II', ver, ver))
                done = True
            elif cmd == LC_VERSION_MIN_MACOSX:   # version, sdk
                f.seek(off + 8)
                f.write(struct.pack('<II', ver, ver))
                done = True
            off += size
        if not done:
            raise Fail('%s has no build version load command' % path)


def bundle_id(game):
    """macOS: pkp's identity (write_identity), one per game folder: the app's identifier with a hash of the folder's full path."""
    return APP_ID + '.game.' + hashlib.sha256(os.path.realpath(game).encode('utf-8')).hexdigest()[:12]


def write_identity(path, game):
    """macOS: a source file that gives pkp an identity: an Info.plist in the section macOS reads it from for a plain executable (pkp is not
    in an app bundle). Metal keeps the shaders it compiles in a cache per identity, and all programs without one share a single cache,
    split into numbered files: a process takes the first file no other running process of that cache holds and looks only in that one.
    The pipelines --prepare-pipelines compiled were then in a file the game often did not get (any other such program running took it),
    and the game compiled them again at every start. With its own identity the game gets the file the preparation filled (the two never
    run at once), and one per game folder keeps the game of another folder (a second install, a test copy) out of it."""
    plist = ('<?xml version="1.0" encoding="UTF-8"?>\n'
             '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
             '<plist version="1.0">\n<dict>\n'
             '\t<key>CFBundleIdentifier</key>\n\t<string>%s</string>\n'
             '\t<key>CFBundleName</key>\n\t<string>%s</string>\n'
             '\t<key>CFBundleInfoDictionaryVersion</key>\n\t<string>6.0</string>\n'
             '</dict>\n</plist>\n') % (bundle_id(game), EXE)
    text = ('// Generated by pkp_build.py (write_identity): the identity of %s on macOS.\n'
            '__attribute__((used, section("__TEXT,__info_plist"))) static const char kInfoPlist[] = %s;\n') % (EXE, json.dumps(plist))
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    return path


def link(runner, obj, game, env):
    progress('link', 0, 1, 'Linking pkp.exe')
    libs = []
    for name, _, _, _, _ in MODULES:
        a = os.path.join(obj, 'pkp_%s.a' % name)
        if os.path.isfile(a):
            os.remove(a)
        objs = sorted(os.path.join(obj, name, f) for f in os.listdir(os.path.join(obj, name)) if f.endswith('.o'))
        rsp = os.path.join(obj, '%s.rsp' % name)
        # UTF-8, not the ANSI code page: zig ar reads the response file as UTF-8, so a game folder path with non-ASCII
        # characters (an accented user name) failed the archive step.
        with open(rsp, 'w', encoding='utf-8') as f:
            f.write('\n'.join('"%s"' % p.replace('\\', '/') for p in objs))
        rc, tail = runner.run([ZIG, 'ar', 'rcs', a, '@' + rsp], env=env)
        if rc != 0:
            raise Fail('Archiving %s failed (exit %d):\n%s' % (name, rc, tail))
        libs.append(a)
    tmp = os.path.join(obj, EXE)
    if MACOS:
        # libpkp_rt.dylib is found next to pkp at run time (@executable_path); the main thread gets the 128 MiB stack the
        # lifted code needs (the Windows exe's /STACK). zig's linker signs the arm64 image ad hoc, which macOS requires.
        cmd = [ZIG, 'c++'] + TARGET + ['-O1', '-fno-exceptions', '-fno-rtti', '-I', SDK, os.path.join(SDK, 'pkp_stub.cpp'),
               write_identity(os.path.join(obj, 'identity.cpp'), game)] + libs + [
            os.path.join(SDK, RT_LIB), '-o', tmp, '-Wl,-rpath,@executable_path', '-Wl,-stack_size,0x8000000']
    else:
        cmd = [ZIG, 'c++'] + TARGET + ['-O1', '-fno-exceptions', '-fno-rtti', '-I', SDK,
               os.path.join(SDK, 'pkp_stub.cpp'), os.path.join(SDK, 'pkp.rc')] + libs + [os.path.join(SDK, RT_LIB),
               '-o', tmp, '-Wl,--stack,134217728', '-Wl,--subsystem,windows']
    rc, tail = runner.run(cmd, env=env, cwd=SDK)
    if rc != 0:
        raise Fail('Linking %s failed (exit %d):\n%s' % (EXE, rc, tail))
    if MACOS:
        macho_set_sdk_version(tmp, 12)
        rc, tail = runner.run(['/usr/bin/codesign', '-s', '-', '-f', tmp], env=env)
        if rc != 0:
            raise Fail('Signing %s failed (exit %d):\n%s' % (EXE, rc, tail))
    else:
        clear_high_entropy_va(tmp)
    dst = os.path.join(game, EXE)
    flush_replace(tmp, dst)                             # done.json then makes it trusted (check())
    progress('link', 1, 1, '%s built' % EXE)
    return dst


# ---- code made again on demand (ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch): runtime core/missed.cpp ----------------
UNIT_EXT = '.dylib' if MACOS else '.dll' if os.name == 'nt' else '.so'   # core/missed.cpp kUnitExt


def missed_stamp(game):
    """The game's missed.txt as the stamp sees it: a new line re-lifts (the address becomes part of the build)."""
    path = os.path.join(game, 'missed.txt')
    return sha256(path) if os.path.isfile(path) else None


def ondemand_key():
    """What the runtime's unit cache is keyed by besides the module build and the address: the lifter, the compiler and its flags."""
    compiler = ','.join(sha256(c) if os.path.isfile(c) else c for c in COMPILER)
    return hashlib.sha256(('%s|%s|%s' % (tree_hash(RECOMP, LIFTER), compiler, ' '.join(CXXFLAGS))).encode('utf-8')).hexdigest()


def write_manifest(missed_dir, game, exefs):
    """build/missed/manifest.tsv for the runtime (core/missed.cpp): the helper command, the unit cache, missed.txt and, per module, its
    base, build id, lift map and input file. Tab separated; relative paths are relative to this folder."""
    rows = ['# PokoMac: how code is made again on demand (pkp_build.py --ondemand; runtime core/missed.cpp)',
            'format\t1', 'lifter_hash\t%s' % ondemand_key(), 'cache\tcache', 'log\tondemand.log',
            'missed\t%s' % os.path.relpath(os.path.join(game, 'missed.txt'), missed_dir),
            # the compiler by its path: the game does not run with the build's environment (PKP_ZIG)
            'command\t%s\t-IB\t%s\t--ondemand\t--zig\t%s' % (sys.executable, os.path.abspath(__file__), os.path.abspath(COMPILER[0]))]
    for name, nso, base, index, _ in MODULES:
        path = os.path.abspath(os.path.join(exefs, nso))
        with open(path, 'rb') as f:
            bid = f.read(0x60)[0x40:0x60].hex()
        rows.append('module\t%s\t%x\t%s\t%s\t%s' % (name, base, bid, os.path.join('map', '%s.json.gz' % name), path))
    os.makedirs(missed_dir, exist_ok=True)
    tmp = os.path.join(missed_dir, 'manifest.tsv.tmp')
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(rows) + '\n')
    os.replace(tmp, os.path.join(missed_dir, 'manifest.tsv'))


def unit_command(src, out, include):
    """The compile command of one unit: the build's COMPILER and CXXFLAGS, as a shared library that exports only its entry."""
    cmd = COMPILER + CXXFLAGS + ['-shared', '-fvisibility=hidden', '-I', include, src, '-o', out]
    if not MACOS and os.name != 'nt':
        cmd.insert(len(COMPILER), '-fPIC')
    return cmd


def unit_env(cache):
    env = dict(os.environ)
    env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(cache, 'zig-cache')
    env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(cache, 'zig-cache')
    return env


def ondemand(argv):
    """--ondemand: make one unit (the runtime runs this while the game waits). Lifts the code at --addr with the module's lift map (or
    --code: bytes at --base, the cheats overlay's patched function, with --sites for its breakpoints), compiles it with COMPILER + CXXFLAGS
    as a shared library and puts it at --out. Exit 0 when --out was written."""
    global COMPILER
    ap = argparse.ArgumentParser(prog='pkp_build.py --ondemand')
    ap.add_argument('--map')
    ap.add_argument('--input')
    ap.add_argument('--code')
    ap.add_argument('--base')
    ap.add_argument('--name', default='code')
    ap.add_argument('--sites', help='--code only: a site list (inline <addr> lines) compiled into the unit, the cheats overlay breakpoints')
    ap.add_argument('--addr', default='0')
    ap.add_argument('--out', required=True)
    ap.add_argument('--log')
    ap.add_argument('--recomp', help='the lifter folder (default: the one next to this script)')
    ap.add_argument('--zig', help='the compiler (default: the bundled Zig)')
    a = ap.parse_args(argv)
    recomp = a.recomp or RECOMP
    if a.zig:
        COMPILER = [a.zig, 'c++']
    log = open(a.log, 'a', encoding='utf-8') if a.log else sys.stdout
    t0 = time.time()

    def say_(text):
        log.write('[%s] %s\n' % (time.strftime('%H:%M:%S'), text))
        log.flush()
    try:
        work = a.out + '.work'
        os.makedirs(work, exist_ok=True)
        addr = int(a.addr, 16)
        # the lifter (recomp nxrecomp/ondemand.py) makes the unit's source in work: lift_code or lift_at, in its own process
        if a.code:
            sites = []
            if a.sites:
                with open(a.sites) as f:
                    sites = [int(t.split()[1], 16) for t in f if t.split() and t.split()[0] == 'inline']
            q = {'op': 'od-code', 'code': a.code, 'base': int(a.base, 16), 'addr': addr, 'name': a.name, 'work': work, 'sites': sites}
        else:
            q = {'op': 'od-at', 'map': a.map, 'addr': addr, 'work': work, 'input': a.input}
        r = lifter_call(recomp, q, work, on_line=say_)
        t1 = time.time()
        tmp = a.out + '.tmp' + os.path.splitext(a.out)[1]
        p = subprocess.run(unit_command(r['source'], tmp, os.path.join(recomp, 'include')), env=unit_env(os.path.dirname(os.path.abspath(a.out))),
                           capture_output=True, text=True, errors='replace')
        if p.returncode != 0:
            say_('compiling %s failed (exit %d):\n%s%s' % (r['source'], p.returncode, p.stdout, p.stderr))
            return 1
        if MACOS:      # ad hoc signature: Apple silicon runs no unsigned code
            subprocess.run(['/usr/bin/codesign', '-s', '-', '-f', tmp], capture_output=True)
        os.replace(tmp, a.out)
        say_('unit %s for 0x%x (%s 0x%x..0x%x, %d instructions): lift %.2fs, compile %.2fs'
             % (os.path.basename(a.out), addr, r['kind'], r['start'], r['end'], r['instructions'], t1 - t0, time.time() - t1))
        shutil.rmtree(work, ignore_errors=True)
        return 0
    except Exception as e:                  # say what it was; the runtime reports the exit code and points at this log
        import traceback
        say_('on-demand unit for %s failed: %s\n%s' % (a.addr, e, traceback.format_exc()))
        return 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--ondemand':
        return ondemand(sys.argv[2:])
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs')
    ap.add_argument('--game')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--cache-mb', type=int, default=CACHE_MB, help='size cap of the compiled-object cache in MiB (0 = off)')
    ap.add_argument('--keep', action='store_true', help='keep the generated sources and objects')
    ap.add_argument('--check', action='store_true', help='only say whether <game>/build/done.json matches these tools')
    ap.add_argument('--live-patch', dest='live_patch', action='store_true', default=True,
                    help='lift every function with the live-patch checks (recomp ctx.h NX_OV / NX_OVL), so the cheats overlay\'s code patches '
                         'reach every caller while the game runs (the default)')
    ap.add_argument('--no-live-patch', dest='live_patch', action='store_false', help=argparse.SUPPRESS)   # only to measure the checks' cost
    a = ap.parse_args()
    if a.check:
        if not a.game:
            ap.error('--check needs --game')
        try:
            current, reason = check(os.path.abspath(a.game))
        except OSError as e:                 # a tool file that cannot be read: Set up then runs and names it
            current, reason = False, 'cannot read the build tools: %s' % e
        print(json.dumps({'current': current, 'reason': reason}), flush=True)
        return 0 if current else 3
    if not a.exefs or not a.game:
        ap.error('--exefs and --game are required')
    game = os.path.abspath(a.game)
    build = os.path.join(game, 'build')
    os.makedirs(build, exist_ok=True)
    cancel_file = os.path.join(build, 'cancel')
    if os.path.exists(cancel_file):
        os.remove(cancel_file)
    t0 = time.time()
    times = {}
    try:
        if not os.path.isfile(ZIG):
            raise Fail('The bundled compiler is missing (%s). Extract the release zip again.' % ZIG)
        want = verify(a.exefs)
        # code mods (docs/MODS.md): their effective ExeFS is lifted in place of the dump's; the dump was verified above
        lift_exefs, mstamp = mods_code(game)
        if lift_exefs:
            missing = [nso for _n, nso, *_ in MODULES if not os.path.isfile(os.path.join(lift_exefs, nso))]
            if missing:
                raise Fail("The code mods' game files (%s) are missing %s. Open Mods and switch a mod off and on again to make them again."
                           % (lift_exefs, ', '.join(missing)))
            say('note', 'Building with the code mods (%s)' % mstamp)
        else:
            lift_exefs = a.exefs
        stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'], 'tools': tool_stamp(a.live_patch),
                 'missed': missed_stamp(game)}
        if mstamp:            # only with code mods: a build without them keeps the stamp it had before mods existed
            stamp['mods'] = mstamp
        state_p = os.path.join(build, 'state.json')
        # another dump or another release (or no readable state): start over. Removing lift/ drops the modules' .lifted
        # markers too, so a changed lifter or main.sites lifts everything again, not only recompiles
        if read_json(state_p) != stamp:
            for d in ('lift', 'obj'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
            write_json(state_p, stamp)
        times['verify'] = round(time.time() - t0, 1)
        runner = Runner(cancel_file)
        env = dict(os.environ)
        # zig's cache lasts one run, as after linking: a stopped run's can hold an object a power cut left as zeros under a manifest that
        # survived, which zig hands back as compiled. The objects themselves are kept in obj/ and objcache/
        zig_cache = os.path.join(build, 'zig-cache')
        shutil.rmtree(zig_cache, ignore_errors=True)
        env['ZIG_GLOBAL_CACHE_DIR'] = zig_cache
        env['ZIG_LOCAL_CACHE_DIR'] = zig_cache
        gen, obj = os.path.join(build, 'lift'), os.path.join(build, 'obj')
        t = time.time()
        missed_dir = os.path.join(build, 'missed')
        lift(runner, lift_exefs, gen, a.jobs, missed_dir, os.path.join(game, 'missed.txt'), a.live_patch)
        write_manifest(missed_dir, game, lift_exefs)       # code made on demand is lifted from the same bytes
        times['lift'] = round(time.time() - t, 1)
        t = time.time()
        compile_all(runner, gen, obj, a.jobs, env, make_cache(os.path.join(build, 'objcache'), a.cache_mb))
        times['compile'] = round(time.time() - t, 1)
        t = time.time()
        exe = link(runner, obj, game, env)
        times['link'] = round(time.time() - t, 1)
        if not a.keep:
            progress('finish', 0, 1, 'Removing the build files')
            for d in ('lift', 'obj', 'zig-cache'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
        done = dict(stamp, exe_sha256=sha256(exe), seconds=times, finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        write_json(os.path.join(build, 'done.json'), done)
        mods_built(game)
        progress('finish', 1, 1, '%s is ready (%d s)' % (EXE, time.time() - t0))
        return 0
    except Cancelled:
        say('cancelled', 'Build paused. Set up again continues where it stopped.')
        return 2
    except Fail as e:
        say('error', str(e).replace('\n', '\\n'))
        return 1
    except Exception as e:            # an unexpected failure: say what it was, plainly
        say('error', '%s: %s' % (type(e).__name__, str(e).replace('\n', '\\n')))
        return 1


if __name__ == '__main__':
    sys.exit(main())
