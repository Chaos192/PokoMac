// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Mods (docs/MODS.md): the installed ones in their order (the first wins), each on or off, what it changes and what it shares with
// another mod; install from a .zip, .7z, .rar or folder; uninstall; and Make a mod (export the game's own files, a template, pack a
// folder to share). The work is mods.py's (src-tauri/src/mods.rs). Code mods need the game built again: Build with mods runs Set up,
// which compiles only the changed code.
import './mods.css';
import { store } from '../store.js';
import { api, askText, confirm, errText, esc, group, icon, toast, toggle } from '../ui.js';
import { gamePhase } from '../action.js';
import { conflictLine, installSummary, modLine, moved, stateNote } from '../mods-model.js';

const btn = (act, label, ic = '', cls = '', disabled = false, data = '') =>
  `<button class="btn sm ${cls}" data-act="${act}" ${data} ${disabled ? 'disabled' : ''}>${ic ? icon(ic) : ''}${esc(label)}</button>`;

/** A row whose label has a second line (what it is, or what a mod changes). */
const item = (label, sub, control, attrs = '') => `<div class="row-item mods-row" ${attrs}>
    <div class="row-label mods-label"><span>${esc(label)}</span>${sub}</div>
    <div class="row-control cluster">${control}</div>
  </div>`;

const note = (text, cls = '') => `<div class="row-item set-note mods-note ${cls}">${icon(cls === 'warn' ? 'triangle-alert' : 'info')}<p>${esc(text)}</p></div>`;

function modRow(m, i, mods, off) {
  const at = `data-mod="${esc(m.id)}"`;
  const conflict = conflictLine(m, mods);
  const lines = [`<span class="mods-sub">${esc(modLine(m))}</span>`];
  if (conflict) lines.push(`<span class="mods-sub warn">${esc(conflict)}</span>`);
  for (const w of m.warnings ?? []) lines.push(`<span class="mods-sub warn">${esc(w)}</span>`);
  const arrow = (act, label, ic, disabled) =>
    `<button class="btn sm quiet icon-only" data-act="${act}" ${at} aria-label="${esc(`${label}: ${m.name}`)}" title="${esc(label)}" ${disabled ? 'disabled' : ''}>${icon(ic)}</button>`;
  const controls = `${arrow('up', 'Move up (wins over more mods)', 'arrow-up', off || i === 0)}
    ${arrow('down', 'Move down', 'arrow-down', off || i === mods.length - 1)}
    ${btn('uninstall', 'Uninstall', 'trash-2', 'quiet', off, at)}
    ${toggle(`mod-on-${m.id}`, m.enabled, { label: `${m.name} on`, disabled: off })}`;
  return item(m.name, lines.join(''), controls, at);
}

export function render(el, ctx) {
  el.innerHTML = `<div class="page set mods">
    <div class="set-head"><h1>Mods</h1></div>
    <div class="set-body" id="mods-body"></div>
  </div>`;
  const body = el.querySelector('#mods-body');
  let st = null;
  let busy = false;
  let error = '';

  const locked = () => {
    const s = store.state;
    if (s.setup?.progress?.state === 'running') return 'Set up is running: the mods can be changed when it has finished.';
    if (['starting', 'running', 'finishing'].includes(gamePhase(s.game))) return 'The game is running: close it to change the mods.';
    return '';
  };

  function draw() {
    const lock = locked();
    const off = busy || !!lock;
    const mods = st?.mods ?? [];
    const head = error ? note(error, 'warn') : note(stateNote(st));
    const build = st?.code_changed
      ? item('Build with mods', '<span class="mods-sub">Builds the game with the code mods that are on (Set up; only the changed code)</span>', btn('build', 'Build', 'wrench', 'primary', off))
      : '';
    body.innerHTML = `
      ${group('Installed', [head, ...mods.map((m, i) => modRow(m, i, mods, off)), build, lock ? note(lock) : ''].filter(Boolean))}
      ${group('Install', [
        item('From a file', '<span class="mods-sub">A .zip, .7z or .rar as it was downloaded</span>', btn('install-file', 'Choose', 'file-archive', '', off)),
        item('From a folder', '<span class="mods-sub">An unpacked mod folder</span>', btn('install-folder', 'Choose', 'folder-open', '', off)),
        item('Mods folder', st ? `<span class="mods-sub path">${esc(st.store)}</span>` : '', btn('open', 'Open', 'folder-open')),
      ])}
      ${group('Make a mod', [
        item('Game files', "<span class=\"mods-sub\">The game's code modules and build ids, and its files if you want them, from your own copy</span>", btn('export', 'Export', 'download', '', busy)),
        item('New mod', '<span class="mods-sub">A mod folder to fill in: romfs, exefs and cheats</span>', btn('template', 'Create', 'plus', '', busy)),
        item('Share', '<span class="mods-sub">Zips a mod folder the way Install reads it</span>', btn('pack', 'Pack', 'package', '', busy)),
      ])}`;
  }

  async function refresh() {
    try {
      st = await api('mods_list');
      error = '';
    } catch (e) {
      error = errText(e);
    }
    draw();
  }

  async function doing(work) {
    busy = true;
    draw();
    try {
      const r = await work();
      if (r && r.mods) st = r;
      else if (r && r.state) st = r.state;
      return r;
    } catch (e) {
      toast(errText(e), 'error');
      return null;
    } finally {
      busy = false;
      draw();
      store.refresh();   // Home's button: Build with mods when the code mods changed
    }
  }

  async function install(folder) {
    const path = await api('mods_pick', { folder }).catch((e) => toast(errText(e), 'error'));
    if (!path) return;
    const r = await doing(() => api('mods_install', { path }));
    if (r) toast(installSummary(r), r.refused?.length ? '' : 'ok', 8000);
  }

  const change = (act, modId, pos = null) => doing(() => api('mods_change', { act, modId, pos }));

  async function exportFiles() {
    const out = await api('mods_pick', { folder: true }).catch(() => null);
    if (!out) return;
    const romfs = await confirm({ title: 'Export the game files too?', body: 'The code modules and build ids are always exported. The game files (RomFS) are large: they are linked, not copied, where the drive allows it.', ok: 'With game files', cancel: 'Code only' });
    const r = await doing(() => api('mods_make', { what: 'export', out, romfs, romfsPath: null }));
    if (r) toast(`Exported to ${r.out}`, 'ok');
  }

  async function template() {
    const name = await askText({ title: 'New mod', label: 'Name', value: 'My mod', ok: 'Create', max: 64 });
    if (!name) return;
    const out = await api('mods_pick', { folder: true }).catch(() => null);
    if (!out) return;
    const r = await doing(() => api('mods_make', { what: 'template', out, name, romfs: false, romfsPath: null }));
    if (r) toast(`Created ${r.out}`, 'ok');
  }

  async function pack() {
    const folder = await api('mods_pick', { folder: true }).catch(() => null);
    if (!folder) return;
    const r = await doing(() => api('mods_pack', { folder }));
    if (r) toast(`Packed ${r.files} files into ${r.out}`, 'ok');
  }

  body.addEventListener('click', async (e) => {
    const b = e.target.closest('[data-act]');
    if (!b || b.disabled) return;
    const modId = b.dataset.mod;
    const mods = st?.mods ?? [];
    switch (b.dataset.act) {
      case 'install-file': return install(false);
      case 'install-folder': return install(true);
      case 'open': return api('mods_open').catch((x) => toast(errText(x), 'error'));
      case 'build': {
        const started = await store.startSetup();
        if (started) ctx.go('home');
        return undefined;
      }
      case 'up':
      case 'down': {
        const to = moved(mods, modId, b.dataset.act === 'up' ? -1 : 1);
        return to === null ? undefined : change('move', modId, to);
      }
      case 'uninstall': {
        const m = mods.find((x) => x.id === modId);
        const yes = await confirm({ title: `Uninstall ${m?.name ?? modId}?`, body: 'Its files are removed from the mods folder. A code mod needs the game built again.', ok: 'Uninstall', danger: true });
        return yes ? change('uninstall', modId) : undefined;
      }
      case 'export': return exportFiles();
      case 'template': return template();
      case 'pack': return pack();
    }
    return undefined;
  });
  body.addEventListener('change', (e) => {
    const input = e.target.closest('input[role="switch"]');
    const modId = input?.closest('[data-mod]')?.dataset.mod;
    if (modId) change(input.checked ? 'enable' : 'disable', modId);
  });

  ctx.on(() => draw());
  draw();
  refresh();
}
