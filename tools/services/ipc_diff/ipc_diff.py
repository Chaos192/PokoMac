#!/usr/bin/env python3
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# SPDX-License-Identifier: GPL-3.0-or-later
"""Service differential test: an old and a new implementation of service sources answer the same requests, byte for byte.

  ipc_diff.py old   --rev <rev> --dest <dir> <repo path> ...     the old files from git into <dir> (written, never printed)
  ipc_diff.py build --build <dir> --alt <dir> [--alt-loud]     configures and builds nxrt_ipc_replay (the tree's sources) and, with
                                                               <dir>'s .cpp files in place of the tree's, nxrt_ipc_replay_alt and
                                                               nxrt_services_test_alt (the service tests on them)
  ipc_diff.py run   --build <dir> --out <dir> <script> ...       replays each script on both, twice each, and compares

The alt files are the other implementation: the old one from git (`old`) once the new one is in the tree, or a new one not in the
tree yet (then the tree still has the old one).

A script is the recorder's format (runtime/src/ipc/record.cpp, [ipc] record: a game's run) or one written by gen_*.py in the same
format. `run` compares only what the guest sees (nxrt_ipc_replay's R lines); a field that differs between two runs of the SAME build
(a time stamp, a random id) is masked and listed, never compared. Exit status 0 when every script matched.
The clean-room rule: the old files are compiled and run, never read. Nothing here prints their contents: a build failure prints the
error lines of the other files only.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
IS_WIN = os.name == "nt"


def cmd_old(a):
    dest = Path(a.dest)
    dest.mkdir(parents=True, exist_ok=True)
    rev = subprocess.run(["git", "-C", str(REPO), "rev-parse", a.rev], capture_output=True, text=True, check=True).stdout.strip()
    lines = [f"rev {rev}"]
    for p in a.paths:
        out = dest / Path(p).name
        with open(out, "wb") as f:
            r = subprocess.run(["git", "-C", str(REPO), "show", f"{rev}:{p}"], stdout=f, stderr=subprocess.PIPE)
        if r.returncode:
            print(f"cannot take {p} at {rev[:10]}", file=sys.stderr)
            return 1
        lines.append(f"{p} {out.stat().st_size} bytes")
    (dest / "MANIFEST.txt").write_text("\n".join(lines) + "\n")
    print(f"{len(a.paths)} old files of {rev[:10]} in {dest}")
    return 0


def vs_env(cache):
    """The Visual Studio clang-cl environment (vcvars64 plus its CMake and Ninja, as shared-rt's build uses), captured once into `cache`
    at normal priority (a few seconds; at Idle on a loaded machine vcvars' own helper processes take minutes)."""
    if cache.exists():
        return dict(l.split("=", 1) for l in cache.read_text().splitlines() if "=" in l)
    inst = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer"
    known = [rf"C:\Program Files\Microsoft Visual Studio\2022\{e}" for e in ("Community", "Professional", "Enterprise", "BuildTools")]
    clang = r"VC\Tools\Llvm\x64\bin\clang-cl.exe"
    vs = [v for v in known if Path(v, clang).exists()]
    if not vs:
        found = subprocess.run([inst + r"\vswhere.exe", "-products", "*", "-sort", "-property", "installationPath"],
                               capture_output=True, text=True).stdout.splitlines()
        vs = [v.strip() for v in found if v.strip() and Path(v.strip(), clang).exists()]
    vs = vs[0]
    ext = vs + r"\Common7\IDE\CommonExtensions\Microsoft\CMake"
    extra = ext + r"\CMake\bin;" + ext + r"\Ninja;" + vs + r"\VC\Tools\Llvm\x64\bin"
    vcvars = vs + r"\VC\Auxiliary\Build\vcvars64.bat"
    out = subprocess.run(f'cmd.exe /s /c "set "PATH={inst};%PATH%" && call "{vcvars}" >nul && set "PATH={extra};%PATH%" && set"',
                         capture_output=True, text=True).stdout
    env = dict(l.split("=", 1) for l in out.splitlines() if "=" in l and not l.startswith("="))
    if "INCLUDE" not in env:
        raise SystemExit("vcvars64 gave no environment")
    cache.write_text("".join(f"{k}={v}\n" for k, v in env.items()))
    return env


def run_build_cmd(cmd, log, env=None):
    with open(log, "ab") as f:
        if IS_WIN:
            # below normal (children inherit the class): someone may be playing on this machine; at Idle a configure alone took over an hour on a busy PC
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env, shell=True,
                               creationflags=0x00004000)   # BELOW_NORMAL_PRIORITY_CLASS
        else:
            r = subprocess.run(["/bin/sh", "-c", cmd], stdout=f, stderr=subprocess.STDOUT)
    return r.returncode


def report_failure(log, old_dir):
    """Error lines of the build, except any that come from (or quote) the old files."""
    old = str(old_dir).replace("\\", "/").lower() if old_dir else None
    shown = 0
    for line in Path(log).read_text(errors="replace").splitlines():
        low = line.replace("\\", "/").lower()
        if old and old in low:
            continue
        if re.search(r"error|FAILED:|undefined|unresolved", line, re.I):
            print("  " + line[:300])
            shown += 1
            if shown >= 40:
                break
    print(f"build failed (log: {log}; lines about the old files are not shown)")


def cmd_build(a):
    b = Path(a.build)
    b.mkdir(parents=True, exist_ok=True)
    log = b / "ipc_diff_build.log"
    log.write_text("")
    alt = []
    if a.alt:
        alt = sorted(str(p).replace("\\", "/") for p in Path(a.alt).iterdir() if p.suffix in (".cpp", ".mm"))
    defs = [f'-DNXRT_IPC_DIFF_ALT="{";".join(alt)}"', f"-DNXRT_IPC_DIFF_ALT_QUIET={'OFF' if a.alt_loud else 'ON'}",
            "-DCMAKE_BUILD_TYPE=Release", "-DNXRT_BUILD_TESTS=ON"]
    defs += [f"-D{d}" for d in a.define]
    gen = '-G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl' if IS_WIN else "-G Ninja"
    env = vs_env(b / "vsenv.txt") if IS_WIN else None
    rc = run_build_cmd(f'cmake -S "{HERE}" -B "{b}" {gen} {" ".join(defs)}', log, env)
    if rc == 0:
        targets = "nxrt_ipc_replay" + (" nxrt_ipc_replay_alt nxrt_services_test_alt" if alt else "") + (" nxrt_services_test" if a.tests else "")
        rc = run_build_cmd(f'cmake --build "{b}" --target {targets} -j {a.jobs}', log, env)
    if rc:
        report_failure(log, None if a.alt_loud else a.alt)
        return 1
    print(f"built in {b}" + (f" (alt: {len(alt)} files)" if alt else ""))
    return 0


def find_exe(build, name):
    for p in [Path(build) / "nxrt" / name, Path(build) / name]:
        for q in (p.with_suffix(".exe"), p):
            if q.is_file():
                return q
    hits = list(Path(build).rglob(name + (".exe" if IS_WIN else "")))
    return hits[0] if hits else None


R_RE = re.compile(r"^R (\d+) (\S+) (\d+) (.*)$")


def parse_out(path):
    rows, tail = {}, []
    for line in Path(path).read_text(errors="replace").splitlines():
        m = R_RE.match(line)
        if m:
            rows[int(m.group(1))] = (m.group(2), int(m.group(3)), m.group(4).split(" "))
        else:
            tail.append(line)
    return rows, tail


def fields(tokens):
    """result, then key=value fields."""
    d = {"result": tokens[0] if tokens else ""}
    for t in tokens[1:]:
        k, _, v = t.partition("=")
        d[k] = v
    return d


def noisy_keys(r1, r2):
    """(seq, field) pairs that differ between two runs of one build."""
    noise = set()
    for seq in set(r1) | set(r2):
        if seq not in r1 or seq not in r2:
            noise.add((seq, "*"))
            continue
        f1, f2 = fields(r1[seq][2]), fields(r2[seq][2])
        for k in set(f1) | set(f2):
            if f1.get(k) != f2.get(k):
                noise.add((seq, k))
    return noise


def first_diff(x, y):
    n = min(len(x), len(y))
    for i in range(n):
        if x[i] != y[i]:
            return i // 2
    return n // 2


def cmd_run(a):
    new_exe, old_exe = find_exe(a.build, "nxrt_ipc_replay"), find_exe(a.build, "nxrt_ipc_replay_alt")
    if not new_exe or not old_exe:
        print("nxrt_ipc_replay / nxrt_ipc_replay_alt not built (ipc_diff.py build --alt ...)")
        return 2
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    bad = 0
    for script in a.scripts:
        name = Path(script).stem
        outs = {}
        for which, exe in (("alt", old_exe), ("tree", new_exe)):
            for n in range(a.runs):
                o = out / f"{name}.{which}{n}.txt"
                tmp = out / f"tmp_{which}{n}"
                p = subprocess.run([str(exe), "--script", script, "--out", str(o), "--tmp", str(tmp), "--timeout", str(a.timeout)],
                                   capture_output=True, text=True, timeout=a.timeout * 50 + 120)
                outs[(which, n)] = parse_out(o) if o.exists() else ({}, [f"no output (exit {p.returncode})"])
        noise = set()
        for which in ("alt", "tree"):
            for n in range(1, a.runs):
                noise |= noisy_keys(outs[(which, 0)][0], outs[(which, n)][0])
        old_rows, old_tail = outs[("alt", 0)]
        new_rows, new_tail = outs[("tree", 0)]
        diffs = []
        for seq in sorted(set(old_rows) | set(new_rows)):
            if (seq, "*") in noise:
                continue
            if seq not in old_rows or seq not in new_rows:
                diffs.append(f"  seq {seq}: only in {'alt' if seq in old_rows else 'tree'}")
                continue
            io, co, to = old_rows[seq]
            fo, fn = fields(to), fields(new_rows[seq][2])
            for k in sorted(set(fo) | set(fn)):
                if (seq, k) in noise or fo.get(k) == fn.get(k):
                    continue
                vo, vn = fo.get(k, "<none>"), fn.get(k, "<none>")
                at = f" (first byte that differs: {first_diff(vo, vn):#x})" if k in ("out",) or k[:1] in "bcw" else ""
                diffs.append(f"  seq {seq} {io} cmd {co} {k}: alt {vo[:96]} tree {vn[:96]}{at}")
        tails_ok = [t for t in old_tail if not t.startswith("#")] == [t for t in new_tail if not t.startswith("#")]
        if not tails_ok:
            diffs.append(f"  end differs: alt {old_tail[-3:]} tree {new_tail[-3:]}")
        masked = sorted(noise)
        status = "MATCH" if not diffs else "DIFFER"
        print(f"{status} {name}: {len(new_rows)} requests, {len(diffs)} differences, {len(masked)} fields masked (vary run to run)")
        for d in diffs[: a.show]:
            print(d)
        if masked and a.verbose:
            for s, k in masked[:40]:
                print(f"  masked seq {s} {k}")
        bad += bool(diffs)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("old")
    p.add_argument("--rev", required=True)
    p.add_argument("--dest", required=True)
    p.add_argument("paths", nargs="+")
    p = sp.add_parser("build")
    p.add_argument("--build", required=True)
    p.add_argument("--alt")
    p.add_argument("--alt-loud", action="store_true", help="compile the alt files with warnings (a new implementation, not an old one)")
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--tests", action="store_true", help="also build nxrt_services_test")
    p.add_argument("--define", action="append", default=[], help="extra CMake definitions (NAME=VALUE)")
    p = sp.add_parser("run")
    p.add_argument("--build", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--runs", type=int, default=2)
    p.add_argument("--timeout", type=int, default=20)
    p.add_argument("--show", type=int, default=30)
    p.add_argument("--verbose", action="store_true")
    p.add_argument("scripts", nargs="+")
    a = ap.parse_args()
    return {"old": cmd_old, "build": cmd_build, "run": cmd_run}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
