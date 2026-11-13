# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch), bc2aacdd's tools/intake/lifter_cli.py; in PokoMac beside the lifter
# (recomp/lifter_cli.py, tools/build/recomp/lifter_cli.py in a release), run by game-build/release_b/pkp_build.py.
"""The lifter (recomp/nxrecomp) as a program of its own. The build script (game-build/release_b/pkp_build.py) runs it as a separate
process and never imports the lifter: the lifter and the intake are one GPL program that runs on its own and talks to the build through
command lines, JSON files and pipes (coord license_inventory.md, group A).

Usage: python -I -B lifter_cli.py <recomp folder> <command> ...
  nxrecomp <argv...>     the lifter's own command line (lift, lift-at, cover: recomp/nxrecomp/__main__.py, `python -m nxrecomp`);
                         its exit code
  nso-segments <module>  the module's three segments decoded (LZ4 or ZBIC: nso.nso_info) and concatenated, on stdout
  call <request.json>    one call, its arguments in the request (a JSON object); its answer as JSON in the file request["result"]:
    {"op": "nros", "sources": [...], "work": D, "table": T, "lifter_hash": H, "nupread_dir": D?, "mod_sources": [...]}
        the NROs the title loads (nros.find, then nros.place, which writes the table) -> {"placed": [{"row", "rel", "key", "nro"}]}:
        each one's row of the table, its path in the RomFS, its unit key (nros.unit_key) and its bytes in the file "nro" (in work)
    {"op": "od-module", "image": M, "base": int, "index": int, "name": N, "work": D, "jobs": int, "before": [...]}
        ondemand.lift_module_unit -> its answer (sources, functions, instructions, ...)
    {"op": "od-code", "code": F, "base": int, "addr": int, "name": N, "work": D, "sites": [int, ...]}
        ondemand.lift_code over the bytes of file F -> its answer (source, kind, start, end, instructions, ...)
    {"op": "od-at", "map": M, "addr": int, "work": D, "input": F?}
        ondemand.lift_at -> its answer (source, kind, start, end, instructions, ...)
  The lifter's log lines go to stdout (UTF-8), one per line, as they come. A call that fails prints the error and its traceback on
  stdout and exits 1 without an answer file. Fields of an answer that JSON cannot carry (sets the callers do not read) are left out."""
import json
import os
import sys
import traceback


def _log(text):
    sys.stdout.write('%s\n' % text)
    sys.stdout.flush()


def _jsonable(d):
    out = {}
    for k, v in d.items():
        try:
            json.dumps(v)
        except (TypeError, ValueError):
            continue
        out[k] = v
    return out


def _nros(q):
    from nxrecomp import nros
    found = nros.find(q['sources'], q['work'], nupread_dir=q.get('nupread_dir'), log=_log, mod_sources=tuple(q.get('mod_sources') or ()))
    placed = []
    for i, (row, data, rel) in enumerate(nros.place(q['table'], found, log=_log)):
        path = os.path.join(q['work'], 'placed_%d.nro' % i)
        with open(path, 'wb') as f:
            f.write(data)
        placed.append({'row': row, 'rel': rel, 'key': nros.unit_key(row, data, q['lifter_hash']), 'nro': path})
    return {'placed': placed}


def _od_module(q):
    from nxrecomp import ondemand
    return ondemand.lift_module_unit(q['image'], q['base'], q['index'], q['name'], q['work'], jobs=q['jobs'], log=_log,
                                     before=q.get('before') or None)


def _od_code(q):
    from nxrecomp import ondemand
    with open(q['code'], 'rb') as f:
        code = f.read()
    return ondemand.lift_code(code, q['base'], q['addr'], q['work'], q['name'], log=_log, sites=q.get('sites') or [])


def _od_at(q):
    from nxrecomp import ondemand
    return ondemand.lift_at(q['map'], q['addr'], q['work'], q.get('input'), log=_log)


OPS = {'nros': _nros, 'od-module': _od_module, 'od-code': _od_code, 'od-at': _od_at}


def call(request_path):
    with open(request_path, encoding='utf-8') as f:
        q = json.load(f)
    try:
        r = OPS[q['op']](q)
    except Exception as e:
        _log('%s failed: %s: %s\n%s' % (q.get('op'), type(e).__name__, e, traceback.format_exc().rstrip()))
        return 1
    tmp = q['result'] + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(_jsonable(r), f)
    os.replace(tmp, q['result'])
    return 0


def nso_segments(path):
    from nxrecomp.nso import nso_info
    with open(path, 'rb') as f:
        data = f.read()
    for seg in nso_info(data)['segments']:
        sys.stdout.buffer.write(seg['data'])
    sys.stdout.buffer.flush()
    return 0


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    recomp, cmd, rest = os.path.abspath(argv[0]), argv[1], argv[2:]
    if recomp not in sys.path:
        sys.path.insert(0, recomp)
    if cmd == 'nxrecomp':
        from nxrecomp.__main__ import main as nx_main
        return nx_main(rest) or 0
    if cmd == 'nso-segments' and len(rest) == 1:
        return nso_segments(rest[0])
    if cmd == 'call' and len(rest) == 1:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace', newline='\n')   # the caller reads UTF-8 lines (a RomFS path may need it)
        return call(rest[0])
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
