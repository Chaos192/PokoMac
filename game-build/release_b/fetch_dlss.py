# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads NVIDIA's nvngx_dlss.dll for the launcher's "Download DLSS" button (release model b).

    python -I tools/build/fetch_dlss.py --dir <install folder>/upscalers/streamline

The package does not carry this file: it is under the NVIDIA RTX SDKs LICENSE (licenses/LICENSE.NVIDIA-RTX-SDKs.txt), whose
redistribution terms do not fit a GPL-3.0 package. The launcher shows that licence and runs this script only when the player asks.
The file comes from NVIDIA's own repository at a fixed tag and must match the pinned sha256 (it is byte-identical to the
bin/x64/nvngx_dlss.dll of the Streamline SDK v2.14.16 release that sl.dlss.dll of this package belongs to).
Output for the launcher, one per line on stdout:  @progress download <done> <total> <text>  /  @error <text>
Exit codes: 0 present and verified, 1 error.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

URL = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll'
SHA256 = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'
SIZE = 58956912
NAME = 'nvngx_dlss.dll'


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    a = ap.parse_args()
    dest = os.path.join(a.dir, NAME)
    part = dest + '.part'
    try:
        os.makedirs(a.dir, exist_ok=True)
        if os.path.isfile(dest) and os.path.getsize(dest) == SIZE and sha256_of(dest) == SHA256:
            say('progress', 'download', SIZE, SIZE, 'nvngx_dlss.dll is already here and matches NVIDIA\'s release.')
            return 0
        say('progress', 'download', 0, SIZE, 'Downloading nvngx_dlss.dll from NVIDIA (github.com/NVIDIA/DLSS, v310.9.16)...')
        h = hashlib.sha256()
        done = 0
        req = urllib.request.Request(URL, headers={'User-Agent': 'PokoMac'})
        with urllib.request.urlopen(req, timeout=30) as r, open(part, 'wb') as out:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                out.write(b)
                h.update(b)
                done += len(b)
                if done > SIZE + (1 << 20):
                    raise RuntimeError('the download is larger than NVIDIA\'s release; stopped')
                say('progress', 'download', done, SIZE, 'Downloading nvngx_dlss.dll from NVIDIA...')
        if done != SIZE or h.hexdigest() != SHA256:
            raise RuntimeError('the downloaded file does not match the pinned NVIDIA release (%d bytes, sha256 %s); it was discarded'
                               % (done, h.hexdigest()))
        os.replace(part, dest)
        say('progress', 'download', SIZE, SIZE, 'nvngx_dlss.dll downloaded and verified.')
        return 0
    except Exception as e:  # noqa: BLE001 - reported to the launcher in one line
        say('error', 'Could not get nvngx_dlss.dll: %s' % e)
        return 1
    finally:
        try:
            os.remove(part)
        except OSError:
            pass


if __name__ == '__main__':
    sys.exit(main())
