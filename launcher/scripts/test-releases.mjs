// Tests for the release list Home and What's New show (src/markdown.js withRunning). Run: node launcher/scripts/test-releases.mjs
import { withRunning } from '../src/markdown.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const v100 = { tag: 'v1.0.0', name: 'PokoMac 1.0.0', date: '2026-11-11' };
const v110 = { tag: 'v1.1.0', name: 'PokoMac 1.1.0', date: '2026-11-15' };

// a release candidate (installed before 1.1.0 is published): it comes first, marked local, and 1.0.0 is not shown as the one you have
const rc = withRunning([v100], '1.1.0');
check(rc.length === 2 && rc[0].local && rc[0].tag === 'v1.1.0' && rc[0].name === 'PokoMac 1.1.0' && rc[1] === v100, 'an unpublished newer version comes first');
// once published, the list is GitHub's own
check(withRunning([v110, v100], '1.1.0').length === 2 && !withRunning([v110, v100], '1.1.0')[0].local, 'a published version adds nothing');
// an older install (an update is out) adds nothing
check(withRunning([v110, v100], '1.0.0').length === 2, 'an older installed version adds nothing');
// dev builds and an unknown version add nothing; an empty list still shows the running version
check(withRunning([v100], '0.1.1').length === 1 && withRunning([v100], '0.0.0').length === 1 && withRunning([v100], '').length === 1, 'dev builds add nothing');
check(withRunning([], '1.1.0').length === 1 && withRunning(undefined, '1.1.0')[0].local, 'no releases loaded: the running version alone');
// Testing groups (virtual entries) are not versions
check(withRunning([{ tag: 'testing', name: 'Testing builds', virtual: true }, v100], '1.1.0')[0].local, 'virtual entries are skipped');
// a newer Testing prerelease listed: the running stable version is older than it, so nothing is added
check(withRunning([{ tag: 'v1.2.0-beta.1', name: 'PokoMac 1.2.0-beta.1', prerelease: true }, v100], '1.1.0').length === 2, 'a newer prerelease listed: nothing added');

console.log(bad ? `${bad} failed` : 'releases: all ok');
process.exit(bad ? 1 : 0);
