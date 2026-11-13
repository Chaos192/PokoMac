// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The mods page's words (docs/MODS.md), kept apart from the page so they can be tested: what a mod changes, which of its files or code
// another mod also changes and which one wins, and what an install did. The mods and their state come from mods.py (src-tauri/src/mods.rs).

export const KIND_LABEL = { romfs: 'Game files', code: 'Code', cheats: 'Cheats', plugins: 'Plugins', frame_rate: 'Frame rate' };

/** What a mod changes, then its version and author when it names them: "Game files · Code · 1.2 · by Someone". */
export function modLine(m) {
  const kinds = (m.kinds ?? []).map((k) => KIND_LABEL[k] ?? k);
  const extra = [m.version ? `Version ${m.version}` : '', m.author ? `by ${m.author}` : ''].filter(Boolean);
  return [...kinds, ...extra].join(' · ');
}

const nameOf = (mods, id) => mods.find((x) => x.id === id)?.name ?? id;

/** One line on what this mod shares with others: how many files and code bytes, and who wins. '' when nothing. */
export function conflictLine(m, mods) {
  const list = m.conflicts ?? [];
  if (!list.length) return '';
  const files = list.filter((c) => c.path);
  const code = list.filter((c) => c.module);
  const won = list.filter((c) => c.winner === m.id).length;
  const others = [...new Set(list.flatMap((c) => c.mods).filter((id) => id !== m.id))].map((id) => nameOf(mods, id));
  const what = [files.length ? `${files.length} ${files.length === 1 ? 'file' : 'files'}` : '', code.length ? `code in ${[...new Set(code.map((c) => c.module))].join(', ')}` : '']
    .filter(Boolean).join(' and ');
  const who = won === list.length ? 'this mod wins' : won === 0 ? `${others.join(', ')} ${others.length === 1 ? 'wins' : 'win'}` : 'the one higher in the list wins each';
  return `Changes ${what} that ${others.join(', ')} also ${others.length === 1 ? 'changes' : 'change'}: ${who}.`;
}

/** The line under the list: whether the next start or a build uses the mods. */
export function stateNote(st) {
  if (!st || !st.mods?.length) return 'No mods yet. Install one from a .zip, .7z or folder: Atmosphere, yuzu and Ryujinx layouts all work.';
  if (st.code_changed) return 'The code mods changed: press Build with mods to build the game again with them. Only the changed code is built again, and your saves and shaders are kept.';
  return 'Game file mods apply the next time the game starts. The higher a mod is in the list, the more it wins over the others.';
}

/** What an install says: the mods it added, then each one it refused and why. */
export function installSummary(r) {
  const ok = (r.installed ?? []).map((m) => m.name);
  const lines = [];
  if (ok.length) lines.push(`Installed ${ok.join(', ')}.`);
  for (const w of (r.installed ?? []).flatMap((m) => m.warnings ?? [])) lines.push(w);
  for (const x of r.refused ?? []) lines.push(`${x.name}: ${x.reason}`);
  return lines.join(' ');
}

/** The position a mod moves to with up (-1) or down (+1), or null at an end. */
export function moved(mods, id, step) {
  const i = mods.findIndex((m) => m.id === id);
  const j = i + step;
  return i < 0 || j < 0 || j >= mods.length ? null : j;
}
