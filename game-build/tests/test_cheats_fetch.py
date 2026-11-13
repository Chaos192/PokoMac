# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""Tests of the cheat fetcher (game-build/release_b/cheats_fetch.py): no network (the sources are served by a fake fetch). What a cheat
file is (labels, master, hex words, 64 KiB, HTML refused), the source order (404, a server error and a refused answer go on to the next),
switch-cheats-db's per-title JSON and its attributions, what lands in the cheats folder (the file with its source labels, toggles.txt
with every cheat off, the .fetched.json record), a player's own file never replaced, up to date / updated, the player's toggles kept,
same-named cheats each with a toggle of their own, 63-byte names.
Run: python -I game-build/tests/test_cheats_fetch.py"""
import json
import os
import shutil
import sys
import tempfile
import urllib.error

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'release_b'))
import cheats_fetch as cf  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails += 1


TID, BID = '0400a3a01ee4c000', '76a489f3bd3642fa'
FILE = ('[Coins 9999]\r\n580F0000 03561930\r\n780F0000 007416AC\r\n640F0000 00000000 0000270F\r\n\r\n'
        '[Water Gun 5x5 area]\n04000000 005A1384 39460BE8\n04000000 005A1384 1486A19F\n')


def fake(answers):
    """A fetch that serves answers {url fragment: bytes | None | Exception} and records the URLs asked."""
    asked = []

    def fetch(url, limit):
        asked.append(url)
        for k, v in answers.items():
            if k in url:
                if isinstance(v, Exception):
                    raise v
                return v
        return None
    return fetch, asked


def test_parse():
    e = cf.parse(FILE)
    check([x[0] for x in e] == [b'Coins 9999', b'Water Gun 5x5 area'] and [x[2] for x in e] == [7, 6], 'parse: names and word counts')
    e = cf.parse('[label]\n[Credits too]\n{Master}\n04000000 00000010 00000001\n[A]\n0400000000000020 00000002\n[trailing label]\n')
    check([(x[0], x[1]) for x in e] == [(b'Master', True), (b'A', False)], 'parse: labels vanish, the master is kept, a 16-digit run is two words')
    for bad, why in (('<!DOCTYPE html><html>', 'HTML'), ('[A]\n0400000 1\n', 'a run that is not whole words'),
                     ('04000000 00000010 00000001\n', 'a word before any name'), ('[A]\n', 'no cheat'), ('[A\n04000000\n', 'no closing ]'),
                     ('{M1}\n04000000\n{M2}\n04000000\n[A]\n04000000\n', 'two masters'), ('[A]\n' + '04000000 ' * 8000, '64 KiB')):
        try:
            cf.parse(bad)
            check(False, 'parse refuses %s' % why)
        except cf.NotCheats:
            check(True, 'parse refuses %s' % why)
    long = 'N' * 70 + 'é'
    check(cf.parse('[%s]\n04000000\n' % long)[0][0] == ('N' * 63).encode(), 'parse: a name keeps its first 63 bytes, as the runtime')


def test_find():
    db = json.dumps({BID.upper(): {'[One]': '[One]\n04000000 00000010 00000001\n\n', '[Two]': '[Two]\n04000000 00000014 00000002'},
                     'attribution': {'Game 1.0 BY someone.txt': 'notes', 'credits BY other.txt': ''}}).encode()
    fetch, asked = fake({'releases/download': None, 'switch-cheats-db': db})
    src, text, author, url = cf.find(TID, BID, cf.SOURCES, fetch)
    check(src and src['id'] == 'switch-cheats-db', 'find: the first source has none (404): the second one\'s')
    check(asked[0] == 'https://github.com/tomvita/MyNXCheats/releases/download/0400A3A01EE4C000/76A489F3BD3642FA.txt',
          'find: MyNXCheats asked first, by its release URL (upper-case ids)')
    check([x[0] for x in cf.parse(text)] == [b'One', b'Two'] and author == 'someone, other', 'find: the db\'s cheats of the build and its authors')
    fetch, asked = fake({'releases/download': urllib.error.URLError('offline'), 'switch-cheats-db': None})
    src, text, author, tried = cf.find(TID, BID, cf.SOURCES, fetch)
    check(src is None and 'no answer' in tried[0] and 'none for this version' in tried[1], 'find: offline and none: nothing, and why')
    fetch, _ = fake({'releases/download': b'<html>a GitHub error page</html>', 'switch-cheats-db': None})
    src, _, _, tried = cf.find(TID, BID, cf.SOURCES, fetch)
    check(src is None and 'refused' in tried[0], 'find: an answer that is no cheat file is refused')
    fetch, _ = fake({'releases/download': FILE.encode()})
    src, text, author, url = cf.find(TID, BID, cf.SOURCES, fetch)
    check(src['id'] == 'mynxcheats' and author == 'TomSwitch (tomvita)', 'find: MyNXCheats has it: taken, its author named')


def test_install():
    d = tempfile.mkdtemp(prefix='cheats_fetch_test_')
    try:
        out = os.path.join(d, 'cheats')
        src = cf.SOURCES[0]
        url = 'https://example/x.txt'
        r = cf.install(out, TID, BID, src, FILE, src['author'], url)
        path = os.path.join(out, BID + '.txt')
        check(r['installed'] and r['cheats'] == 2 and r['file'] == path, 'install: the file is in the cheats folder')
        body = open(path, encoding='utf-8').read()
        check(body.startswith('[Cheats for 0400A3A01EE4C000 build 76A489F3BD3642FA]\n[By TomSwitch (tomvita)]\n'), 'install: the source labels first')
        check([x[0] for x in cf.parse(body)] == [b'Coins 9999', b'Water Gun 5x5 area'], 'install: the labels are no cheats; the cheats are the same')
        check(all(len(l.encode()) <= 65 for l in body.splitlines()[:4]), 'install: each label within 63 bytes')
        tog = open(os.path.join(out, 'toggles.txt'), 'rb').read()
        check(tog == b'[Coins 9999]\nfalse\n[Water Gun 5x5 area]\nfalse\n', 'install: toggles.txt has every cheat off')
        meta = json.load(open(os.path.join(out, BID + '.fetched.json'), encoding='utf-8'))
        check(meta['author'] == 'TomSwitch (tomvita)' and meta['url'] == url and meta['cheats'] == 2 and len(meta['sha256']) == 64,
              'install: the record names the source, the author and the file')
        r = cf.install(out, TID, BID, src, FILE, src['author'], url)
        check(not r['installed'] and 'Up to date' in r['message'], 'install: the same file again: up to date, nothing written')
        # the player turned Coins on; the author adds a cheat: the update keeps Coins on, the new cheat comes in off
        open(os.path.join(out, 'toggles.txt'), 'wb').write(b'[Coins 9999]\ntrue\n[Water Gun 5x5 area]\nfalse')
        r = cf.install(out, TID, BID, src, FILE + '[New]\n04000000 00000010 00000001\n', src['author'], url)
        tog = open(os.path.join(out, 'toggles.txt'), 'rb').read()
        check(r['installed'] and r['updated'] and tog == b'[Coins 9999]\ntrue\n[Water Gun 5x5 area]\nfalse\n[New]\nfalse\n',
              'install: an update keeps the player\'s toggles and adds the new cheat off')
        # same-named cheats: each gets a toggle of its own
        r = cf.install(out, TID, BID, src, FILE + '[New]\n04000000 00000010 00000001\n[New]\n04000000 00000014 00000001\n', src['author'], url)
        check(open(os.path.join(out, 'toggles.txt'), 'rb').read().count(b'[New]\nfalse') == 2, 'install: two cheats of one name, two toggles')
        # a player's own file for the version is never replaced
        own = os.path.join(d, 'own')
        os.makedirs(own)
        open(os.path.join(own, BID + '.txt'), 'w').write('[Mine]\n04000000 00000010 00000001\n')
        r = cf.install(own, TID, BID, src, FILE, src['author'], url)
        check(not r['installed'] and open(os.path.join(own, BID + '.txt')).read().startswith('[Mine]') and
              not os.path.exists(os.path.join(own, 'toggles.txt')), 'install: a file of the player\'s own is left as it is')
        tree = os.path.join(d, 'tree')
        os.makedirs(os.path.join(tree, TID, 'cheats'))
        open(os.path.join(tree, TID, 'cheats', BID + '.txt'), 'w').write('[Mine]\n04000000 00000010 00000001\n')
        r = cf.install(tree, TID, BID, src, FILE, src['author'], url)
        check(not r['installed'] and not os.path.exists(os.path.join(tree, BID + '.txt')), 'install: the console layout\'s own file counts too')
    finally:
        shutil.rmtree(d)


def test_main_args():
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        cf.main(['--title', 'nothex', '--build', BID, '--out', 'x'])
    check('error' in json.loads(buf.getvalue()), 'main: a title id that is not hex is an error, nothing fetched')


test_parse()
test_find()
test_install()
test_main_args()
print('ALL CHEATS FETCH TESTS PASSED' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
