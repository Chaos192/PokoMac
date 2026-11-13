# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Copyright (c) 2026 Chaos192
"""Finds the public Atmosphère cheat file of a game's version and puts it in the game's cheats folder, every cheat OFF.

    python -I tools/build/cheats_fetch.py --title <program id> --build <main's build id> --out <cheats folder> [--force]

Cheat files are not bundled with the app: their authors publish them without a licence to redistribute.
The launcher runs this on the player's machine for the game and version they have, the way the console's cheat managers (Breeze, EdiZon,
the AIO switch updater) download them, and shows whose they are. Sources, in order, the first that has the build wins:
  1. tomvita's MyNXCheats (TomSwitch on GBAtemp, Breeze's author): a release per title, its asset <BUILD ID>.txt
     https://github.com/tomvita/MyNXCheats/releases/download/<TITLE ID>/<BUILD ID>.txt
  2. switch-cheats-db (HamletDuFromage's collection of GBAtemp's and other authors' cheats, with their attributions)
     https://raw.githubusercontent.com/HamletDuFromage/switch-cheats-db/master/cheats/<TITLE ID>.json  ({build id: {"[name]": text}})
Only a plain Atmosphère cheat file is taken: [name] / {name} headers and 8-digit hex words, under 64 KiB (what a console accepts); anything
else is refused and nothing is written. Nothing in it is run here; the game's cheat engine runs it, off until the player turns a cheat on.
What it writes into --out (the runtime reads <out>/<build id>.txt and toggles.txt beside it, docs/CHEATS.md):
  <build id>.txt            the file, after a few label lines naming its source and author (a [label] with no code words is no cheat)
  toggles.txt               false for every cheat (cheats the player already switched keep their setting): all OFF
  <build id>.fetched.json   where it came from (source, author, page, licence, sha256 of what was downloaded, date): the launcher shows it
A cheat file of the player's own for that version (one without .fetched.json) is never replaced. A file fetched earlier is replaced only
when the source changed it (or with --force).
Output: one JSON object on stdout: {"found": true, "file", "source", "author", "home", "url", "license", "cheats", "names", "updated",
"message"} or {"found": false, "message", "tried": [...]}; {"error": "..."} when the arguments or the folder are wrong. Exit code 0.
Only the title id and build id go to these hosts (GitHub); nothing else about the player or the machine.
Shared by AnySwitch and PokoMac (stdlib only; ANYSWITCH_CHEATS_SOURCES may point the sources at a test server: a JSON list like SOURCES).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request

MAX_TEXT = 0x10000                 # a console refuses a cheat file of 64 KiB or more
MAX_DB = 8 << 20                   # a switch-cheats-db title file (every build of the title)
NAME_MAX = 0x3F                    # a cheat's name keeps its first 63 bytes (dmnt; the runtime's ParseFile)
TIMEOUT = 15
AGENT = 'AnySwitch-cheats/1'

SOURCES = [
    {'id': 'mynxcheats', 'name': "tomvita's MyNXCheats", 'author': 'TomSwitch (tomvita)', 'home': 'https://github.com/tomvita/MyNXCheats',
     'license': 'none stated (the author publishes the cheats for free use; not redistributed by this app)', 'kind': 'txt',
     'url': 'https://github.com/tomvita/MyNXCheats/releases/download/{title}/{build}.txt'},
    {'id': 'switch-cheats-db', 'name': 'switch-cheats-db', 'author': 'the authors it credits (collected by HamletDuFromage)',
     'home': 'https://github.com/HamletDuFromage/switch-cheats-db', 'license': 'none stated (the authors\' own cheats, collected)',
     'kind': 'db', 'url': 'https://raw.githubusercontent.com/HamletDuFromage/switch-cheats-db/master/cheats/{title}.json'},
]


class NotCheats(ValueError):
    pass


def hex_id(s, digits):
    s = (s or '').strip().lower()
    if s.startswith('0x'):
        s = s[2:]
    if not re.fullmatch(r'[0-9a-f]+', s) or len(s) < digits:
        raise ValueError('not a hex id: %r' % s)
    return s[:digits]


def parse(text):
    """The cheats of an Atmosphère cheat file, as the runtime's ParseFile reads it: [(name bytes, master, words)]. NotCheats if it is
    not one (HTML, binary, a stray character, a word that is not 8 hex digits, no cheat at all)."""
    if len(text.encode('utf-8')) >= MAX_TEXT:
        raise NotCheats('the file is 64 KiB or larger; a console refuses it')
    if '\0' in text:
        raise NotCheats('not a text file')
    out, cur, i, n = [], None, 0, len(text)
    while i < n:
        c = text[i]
        if c in ' \t\r\n':
            i += 1
            continue
        if c in '[{':
            close = ']' if c == '[' else '}'
            j = i + 1
            while j < n and text[j] not in (close, '\n'):
                j += 1
            if j >= n or text[j] != close:
                raise NotCheats('a cheat name has no closing %s' % close)
            name = text[i + 1:j].encode('utf-8')[:NAME_MAX]
            if cur is not None and cur[2] == 0:
                out.pop()                              # a header with no words is a label: the next one takes its place
            cur = [name, c == '{', 0]
            out.append(cur)
            i = j + 1
            continue
        if c in '0123456789abcdefABCDEF':
            j = i
            while j < n and text[j] in '0123456789abcdefABCDEF':
                j += 1
            if (j - i) % 8:
                raise NotCheats('a run of %d hex digits is not whole 8-digit words' % (j - i))
            if cur is None:
                raise NotCheats('a code word before any cheat name')
            cur[2] += (j - i) // 8
            i = j
            continue
        raise NotCheats('not an Atmosphère cheat file (a %r where a name or a code word goes)' % c)
    if out and out[-1][2] == 0:
        out.pop()
    if not [e for e in out if not e[1]]:
        raise NotCheats('no cheat in it')
    if sum(1 for e in out if e[1]) > 1:
        raise NotCheats('two master codes {...}; a console refuses the file')
    return [tuple(e) for e in out]


def get(url, limit):
    """The body at url (None: not there, 404), at most limit bytes. Raises on other failures."""
    req = urllib.request.Request(url, headers={'User-Agent': AGENT})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            body = r.read(limit + 1)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    if len(body) > limit:
        raise NotCheats('the answer is larger than a cheat file can be')
    return body


def from_db(body, build, src):
    """switch-cheats-db's title file: the cheats of one build as one file text, and the authors its attribution names."""
    db = json.loads(body.decode('utf-8'))
    got = next((v for k, v in db.items() if k.lower() == build.lower() and isinstance(v, dict)), None)
    if not got:
        return None, None
    text = ''.join(v if v.endswith('\n') else v + '\n' for v in got.values() if isinstance(v, str))
    authors = []
    for k in (db.get('attribution') or {}):
        m = re.search(r'\bBY\s+(.+?)(?:\.txt)?$', str(k), re.I)
        if m and m.group(1).strip() not in authors:
            authors.append(m.group(1).strip())
    return text, (', '.join(authors[:6]) if authors else src['author'])


def find(title, build, sources, fetch=get):
    """The first source's cheat file for title/build: (source, text, author, url) or (None, None, None, tried)."""
    tried = []
    for src in sources:
        url = src['url'].format(title=title.upper(), build=build.upper())
        try:
            body = fetch(url, MAX_TEXT if src['kind'] == 'txt' else MAX_DB)
            if body is None:
                tried.append('%s: none for this version' % src['name'])
                continue
            author = src['author']
            if src['kind'] == 'db':
                text, author = from_db(body, build, src)
                if text is None:
                    tried.append('%s: none for this version' % src['name'])
                    continue
            else:
                text = body.decode('utf-8')
            parse(text)
            return src, text, author, url
        except NotCheats as e:
            tried.append('%s: refused (%s)' % (src['name'], e))
        except (UnicodeDecodeError, ValueError) as e:
            tried.append('%s: refused (%s)' % (src['name'], e))
        except Exception as e:  # noqa: BLE001 - offline, a server error: try the next source, say so
            tried.append('%s: no answer (%s)' % (src['name'], e))
    return None, None, None, tried


def label(s):
    s = s.replace(']', ')').replace('[', '(').replace('\n', ' ')
    return '[' + s.encode('utf-8')[:NAME_MAX].decode('utf-8', 'ignore') + ']'


def read_toggles(path):
    """toggles.txt's names (bytes) in order, with the line that follows each."""
    try:
        data = open(path, 'rb').read()
    except OSError:
        return None
    return re.findall(rb'\[([^\]\n]*)\]', data)


def install(out, title, build, src, text, author, url, force=False, now=None):
    """Writes the file, the toggles (all off) and the source record into out. Returns the answer for the launcher."""
    os.makedirs(out, exist_ok=True)
    bid = build.lower()
    path = os.path.join(out, bid + '.txt')
    meta_path = os.path.join(out, bid + '.fetched.json')
    tree = os.path.join(out, title.lower(), 'cheats', bid + '.txt')
    sha = hashlib.sha256(text.encode('utf-8')).hexdigest()
    meta_old = None
    if os.path.isfile(meta_path):
        try:
            meta_old = json.load(open(meta_path, encoding='utf-8'))
        except (OSError, ValueError):
            meta_old = None
    if os.path.isfile(tree) or (os.path.isfile(path) and meta_old is None):
        mine = tree if os.path.isfile(tree) else path
        return {'found': True, 'installed': False, 'file': mine,
                'message': 'This version already has a cheat file of your own (%s); it was left as it is.' % mine}
    if meta_old and meta_old.get('sha256') == sha and os.path.isfile(path) and not force:
        msg = 'Up to date: %d cheats from %s, by %s.' % (meta_old.get('cheats', 0), meta_old.get('source_name', src['name']), meta_old.get('author', author))
        return dict(answer(meta_old, path), installed=False, updated=False, message=msg)
    entries = parse(text)
    when = (now or datetime.datetime.now(datetime.timezone.utc)).strftime('%Y-%m-%d')
    head = [label('Cheats for %s build %s' % (title.upper(), build.upper())), label('By %s' % author),
            label('From %s' % src['home'].replace('https://', '')),
            label('Fetched %s; licence: %s' % (when, src['license'].split(' (')[0]))]
    body = '\n'.join(head) + '\n\n' + text.replace('\r\n', '\n')
    tmp = path + '.part'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(body)
    os.replace(tmp, path)
    # toggles: every cheat off, the player's earlier choices kept (by name, as the console's toggles.txt)
    tog = os.path.join(out, 'toggles.txt')
    have = read_toggles(tog)
    names = [e[0] for e in entries if not e[1]]
    if have is None:
        data = b''.join(b'[' + nm + b']\nfalse\n' for nm in names)
        with open(tog, 'wb') as f:
            f.write(data)
    else:
        # same-named cheats take toggles.txt's entries of that name in order (the runtime, 8fe4db7b): each needs one of its own
        left = {}
        for nm in have:
            left[nm] = left.get(nm, 0) + 1
        add = []
        for nm in names:
            if left.get(nm, 0) > 0:
                left[nm] -= 1
            else:
                add.append(nm)
        if add:
            prev = open(tog, 'rb').read()
            with open(tog, 'ab') as f:
                if prev and not prev.endswith(b'\n'):
                    f.write(b'\n')
                f.write(b''.join(b'[' + nm + b']\nfalse\n' for nm in add))
    meta = {'source': src['id'], 'source_name': src['name'], 'author': author, 'home': src['home'], 'url': url,
            'license': src['license'], 'title': title.upper(), 'build': build.upper(), 'fetched': when, 'sha256': sha,
            'cheats': len(names), 'names': [nm.decode('utf-8', 'replace') for nm in names]}
    with open(meta_path, 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=1, ensure_ascii=False)
    updated = meta_old is not None
    msg = '%s %d cheats from %s, by %s. All are off: turn them on in the game\'s cheats menu.' % (
        'Updated:' if updated else 'Added', len(names), src['name'], author)
    return dict(answer(meta, path), installed=True, updated=updated, message=msg)


def answer(meta, path):
    return {'found': True, 'file': path, 'source': meta.get('source'), 'source_name': meta.get('source_name'),
            'author': meta.get('author'), 'home': meta.get('home'), 'url': meta.get('url'), 'license': meta.get('license'),
            'cheats': meta.get('cheats'), 'names': meta.get('names', []), 'fetched': meta.get('fetched')}


def sources():
    env = os.environ.get('ANYSWITCH_CHEATS_SOURCES', '').strip()
    return json.loads(env) if env else SOURCES


def main(argv=None):
    ap = argparse.ArgumentParser(description='Fetch the public cheat file of a game version (every cheat off).')
    ap.add_argument('--title', required=True)
    ap.add_argument('--build', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args(argv)
    try:
        title, build = hex_id(a.title, 16), hex_id(a.build, 16)
    except ValueError as e:
        print(json.dumps({'error': str(e)}))
        return 0
    src, text, author, url = find(title, build, sources())
    if src is None:
        print(json.dumps({'found': False, 'tried': url, 'message': 'No public cheat file for this version (%s, build %s).' %
                          (title.upper(), build.upper())}, ensure_ascii=False))
        return 0
    try:
        print(json.dumps(install(a.out, title, build, src, text, author, url, a.force), ensure_ascii=False))
    except OSError as e:
        print(json.dumps({'error': 'The cheats folder could not be written: %s' % e}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
