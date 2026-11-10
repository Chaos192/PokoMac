// Settings: draws settings-schema.js and saves every change by itself. The schema says what exists; this file only knows how to draw
// each row type, when to write, and what the buttons do. write_settings and save_config go through one queue, in order.
//
// A redraw must never pull a control from under the player: draw() waits while a text field has focus or the pointer is down, and
// otherwise replaces only the rows whose markup changed. Writes are shown at once and undone from the files if they fail.
import './settings.css';
import { api, askText, confirm, errText, esc, field, group, icon, row, segmented, select, slider, toast, toggle } from '../ui.js';
import { askCompact, store } from '../store.js';
import { backupDetail, backupNow, backupText, runRestore } from './backups.js';
import { canUpdate, gamePhase } from '../action.js';
import { runRepair, runUndo } from './repair.js';
import { askTesting } from './testing-notice.js';
import { reportProblem } from '../report.js';
import { confirmBuild, loadBuilds, pickBuild } from './builds.js';
import {
  DLSS_DOWNLOAD, IMPORT_FACTS, SECTIONS, applyEdits, customStart, fitNumber, isAvailable, isEnabled, makeSource, optionsFor, readAll, readField,
  rowsOf, shownChoice, splitEdits, writeField, FIELDS,
} from '../settings-schema.js';

const SLIDER_WAIT_MS = 250;
const TEXT_WAIT_MS = 600;
const SAVED_SHOW_MS = 1400;

let lastSection = 'display';   // coming back to Settings lands where the player was

const fieldOf = (t) => {
  const [id, part] = (t.type === 'radio' ? t.name : t.id).split('--');
  return { f: FIELDS[id], part };
};

export async function render(el, ctx) {
  const [dump, cfg, dlssInfo, overviewInfo] = await Promise.all([
    api('read_settings'),
    api('get_config'),
    api('dlss_status').catch(() => null),
    api('get_overview').catch(() => null),
  ]);
  let src = makeSource(dump, cfg);
  let dlss = dlssInfo;
  let overview = overviewInfo;
  let gameDir = dump.game_dir;
  let sources = [];
  let profiles = { active: 'default', profiles: [] };
  let repairBackup = null;   // the last repair's backup (name, files), when Undo has something to put back
  let backups = null;   // savebackup.rs Status: the settings, how many copies and the newest
  let steamBusy = false;
  let steam = null;     // steam.rs Status: Steam found and running, and whether the game and the launcher shortcuts are in it
  let icloud = null;    // icloud.rs Status: whether this Mac has iCloud Drive, and when the save last synced
  let section = store.state.dlss ? 'graphics' : ctx.section || lastSection;

  const customOpen = new Set();   // choice rows showing their number field although the value is a preset
  const pending = new Map();      // field id -> { timer, run }: debounced commits
  const sig = new Map();          // row id -> the markup it was drawn from
  let queue = Promise.resolve();
  let unsaved = [];               // edits in the queue, not on disk yet
  let pointerDown = false;
  let redraw = false;
  let importing = false;
  let importResult = null;
  let checking = false;
  let checked = false;
  let savedTimer = null;
  let wasActive = false;
  let hadDlss = !!store.state.dlss;

  function env() {
    const s = store.state;
    const phase = gamePhase(s.game);
    const gameBusy = phase === 'starting' || phase === 'running' || phase === 'finishing';
    const update = s.news.update;
    return {
      gameDir,
      defaultGameDir: overview?.default_game_dir ?? '',
      gameFiles: src.config.dump_dir || s.app?.dump?.path || '',
      version: s.app?.version ?? '',
      gameBusy,
      canSetUp: !!s.app?.dump && !gameBusy && !s.task && !s.starting,
      storage: s.storage,
      repack: s.repack,
      taskBusy: !!s.task || s.starting,
      saveSources: sources,
      profiles,
      repairBackup,
      backups,
      backupLine: backupText(backups),
      steam,
      steamBusy,
      icloud,
      backupDetail: backupDetail(backups),
      importing,
      importResult,
      dlssOffered: !!(dlss?.available || dlss?.present),
      dlssCanFetch: !!dlss?.available,
      dlssPresent: !!dlss?.present,
      dlssBusy: !!s.dlss,
      canUpdate: canUpdate(s),
      updating: !!s.update,
      checking,
      updateText: checking ? 'Checking'
        : update ? `Version ${update.tag.replace(/^v/, '')} is available`
        : !checked ? ''
        : s.news.releases.length ? 'You have the latest version'
        : 'Could not reach GitHub',
    };
  }

  // ---- drawing ------------------------------------------------------------------------------------------------------------------

  const numberField = (id, value, min, max, label, disabled) =>
    field(id, value, '', { type: 'number', label, disabled }).replace('class="input', `min="${min}" max="${max}" step="1" class="input set-num`);

  const clockInput = (minutes) => {
    const picked = new Date(Date.now() + minutes * 60000);
    return new Date(picked.getTime() - picked.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
  };

  const clockReadout = (minutes) => {
    if (!minutes) return 'Matches this PC';
    const left = Math.abs(minutes);
    const days = Math.floor(left / 1440), hours = Math.floor(left % 1440 / 60), mins = left % 60;
    const parts = [...(days ? [`${days} d`] : []), ...(hours ? [`${hours} h`] : []), ...(mins ? [`${mins} min`] : [])];
    return `${parts.join(' ')} ${minutes > 0 ? 'ahead of' : 'behind'} this PC`;
  };

  const CONTROL = {
    segmented: (f, value, e, off) => segmented(f.id, optionsFor(f, value, e), value, { disabled: off }),
    select: (f, value, e, off) => select(f.id, optionsFor(f, value, e), shownChoice(f, value), { disabled: off }),
    toggle: (f, value, e, off) => toggle(f.id, value, { disabled: off }),
    slider: (f, value, e, off) => slider(f.id, f.min, f.max, value, { step: f.step, suffix: f.suffix ?? '', disabled: off }),
    choice: (f, value, e, off) => {
      const open = customOpen.has(f.id) || shownChoice(f, value) === 'custom';
      const c = f.custom;
      return `${select(f.id, optionsFor(f, value, e), open ? 'custom' : value, { disabled: off })}
        ${open ? `${numberField(`${f.id}--custom`, customStart(f, value), c.min, c.max, c.label, off)}<span class="set-unit">${esc(c.suffix)}</span>` : ''}`;
    },
    size: (f, value, e, off) => {
      const open = customOpen.has(f.id) || shownChoice(f, value) === 'custom';
      const [w, h] = String(value).split('x');
      return `${select(f.id, optionsFor(f, value, e), open ? 'custom' : value, { disabled: off })}
        ${open ? `${numberField(`${f.id}--w`, w, f.min, f.max, 'Window width', off)}<span class="set-unit">x</span>${numberField(`${f.id}--h`, h, f.min, f.max, 'Window height', off)}` : ''}`;
    },
    path: (f, value, e, off) => {
      const holder = f.placeholder(e);
      return `${field(f.id, value, holder, { mono: true, disabled: off }).replace('class="input', `title="${esc(value || holder)}" class="input set-path`)}
        <button class="btn" data-act="browse" data-arg="${f.id}">Browse</button>
        ${f.open ? `<button class="btn" data-act="open" data-arg="${f.open}">${icon('folder-open')}Open</button>` : ''}`;
    },
    clock: (f, value, e, off) => `${field(f.id, clockInput(value), '', { type: 'datetime-local', disabled: off }).replace('class="input', 'step="60" class="input')}
      <button class="btn" data-act="clock-real" data-arg="${f.id}" ${off ? 'disabled' : ''}>${icon('rotate-ccw')}Real time</button>
      <span class="set-value">${esc(clockReadout(value))}</span>`,
  };

  const actionsOf = (r, e) => (typeof r.actions === 'function' ? r.actions(e) : r.actions ?? []);

  const actionButton = (a) => `<button class="btn ${a.primary ? 'primary' : ''}" data-act="${a.act}" ${a.arg ? `data-arg="${esc(a.arg)}"` : ''} ${a.name ? `data-name="${esc(a.name)}"` : ''} ${a.disabled ? 'disabled' : ''}>${icon(a.icon)}${esc(a.label)}</button>`;

  function infoControl(r, e) {
    const value = typeof r.value === 'function' ? r.value(e) : r.value;
    const text = value ? `<span class="set-value clip ${r.mono ? 'path' : ''} ${r.selectable ? 'selectable' : ''}" title="${esc(value)}">${esc(value)}</span>` : '';
    return text + actionsOf(r, e).map(actionButton).join('');
  }

  const rowHtml = (r, cls, label, control) => row(label, control).replace('class="row-item"', `class="row-item ${cls}" data-row="${r.id}"`);

  function noteHtml(r, e) {
    const n = r.note(e);
    return `<div class="row-item set-note ${n.kind ?? ''}" data-row="${r.id}">
      ${n.kind ? icon(n.kind === 'danger' ? 'circle-x' : 'circle-check') : ''}
      <p>${esc(n.text)}${n.path ? ` <span class="path selectable">${esc(n.path)}</span>` : ''}</p>
    </div>`;
  }

  function rowOf(r, values, e) {
    if (r.type === 'note') return noteHtml(r, e);
    if (r.type === 'keycap') return rowHtml(r, '', r.label, `<kbd class="set-key">${esc(r.key)}</kbd>`);
    if (r.type === 'info') return rowHtml(r, 'set-info', r.label, infoControl(r, e));
    const off = !isEnabled(r, src, e);
    return rowHtml(r, r.type === 'path' ? 'set-pathrow' : '', r.label, CONTROL[r.type](r, values[r.id], e, off));
  }

  function bannerHtml(sec, values, e) {
    const waiting = sec.groups.some((g) => rowsOf(g, values, e).some((r) => r.at && !isAvailable(r, src)));
    const noExe = sec.id === 'game' && ['no_exe', 'no_runtime'].includes(store.state.app?.readiness?.kind);
    const text = waiting ? 'Available after the first start of the game'
      : noExe ? store.state.app.readiness.detail
      : e.gameBusy && sec.id !== 'about' ? 'Changes apply next time the game starts'
      : '';
    return text ? `<div class="banner ${noExe && !waiting ? 'warn' : 'info'}" data-banner="${esc(text)}">${icon(noExe && !waiting ? 'triangle-alert' : 'info')}<div>${esc(text)}</div></div>` : '';
  }

  function sectionHtml() {
    const sec = SECTIONS.find((s) => s.id === section);
    const values = readAll(src);
    const e = env();
    const groups = sec.groups.map((g) => {
      const rows = rowsOf(g, values, e);
      return rows.length ? group(g.title, rows.map((r) => rowOf(r, values, e))) : '';
    });
    return bannerHtml(sec, values, e) + groups.join('');
  }

  const body = () => el.querySelector('#set-body');
  const rowsIn = (node) => [...node.querySelectorAll('[data-row]')];
  const shape = (node) => [...rowsIn(node).map((r) => r.dataset.row), node.querySelector('[data-banner]')?.dataset.banner].join('|');
  const rowSig = (node) => node.outerHTML.replace(/row-\d+/g, '');

  function focusKey() {
    const a = document.activeElement;
    if (!a || !el.contains(a) || a.closest('.set-pills')) return null;
    if (a.id) return `#${CSS.escape(a.id)}`;
    if (a.name) return `input[name="${CSS.escape(a.name)}"][value="${CSS.escape(a.value)}"]`;
    if (a.dataset.act) return `[data-act="${a.dataset.act}"]${a.dataset.arg ? `[data-arg="${CSS.escape(a.dataset.arg)}"]` : ''}`;
    return null;
  }

  const typing = () => document.activeElement?.matches?.('input.input') && el.contains(document.activeElement);

  /** Brings the section up to date. force: everything, now, even under the cursor (a failed write, a new section). */
  function draw(force = false) {
    if (!force && (pointerDown || typing())) {
      redraw = true;
      return;
    }
    redraw = false;
    const host = body();
    const next = document.createElement('div');
    next.innerHTML = sectionHtml();
    const key = focusKey();
    if (force || shape(host) !== shape(next)) {
      host.replaceChildren(...next.childNodes);
      sig.clear();
      rowsIn(host).forEach((r) => sig.set(r.dataset.row, rowSig(r)));
    } else {
      const fresh = rowsIn(next);
      rowsIn(host).forEach((old, i) => {
        const was = sig.get(old.dataset.row);
        const now = rowSig(fresh[i]);
        if (was === now) return;
        sig.set(old.dataset.row, now);
        old.replaceWith(fresh[i]);
      });
    }
    if (key && !el.contains(document.activeElement)) host.querySelector(key)?.focus();
  }

  function saved() {
    const tick = el.querySelector('#set-saved');
    tick.innerHTML = `${icon('check')}Saved`;
    tick.classList.add('show');
    clearTimeout(savedTimer);
    savedTimer = setTimeout(() => tick.classList.remove('show'), SAVED_SHOW_MS);
  }

  // ---- saving -------------------------------------------------------------------------------------------------------------------

  /** Whatever the files say now. Runs inside the queue, so it must not wait for it; from outside see reload. */
  async function readBack(force = false) {
    const [d, c, s, o] = await Promise.all([
      api('read_settings'),
      api('get_config'),
      api('dlss_status').catch(() => dlss),
      api('get_overview').catch(() => overview),
    ]);
    src = unsaved.reduce(applyEdits, makeSource(d, c));   // what is still on its way stays on screen
    gameDir = d.game_dir;
    dlss = s;
    overview = o;
    draw(force);
  }

  const reload = () => (queue = queue.then(() => readBack()));

  async function persist(edits, reloads) {
    const { ini, config } = splitEdits(edits);
    if (ini.length) await api('write_settings', { edits: ini });
    if (config) await api('save_config', { config: { ...(await api('get_config')), ...config } });
    if (config && 'channel' in config) await store.refreshNews(true);   // another channel, another list of releases
    if (config && 'icloud_sync' in config) refreshIcloud();
    if (reloads) await store.refresh();
    saved();
  }

  /** Gives field f this value: on screen at once, in the files through the queue, back as it was if that fails. */
  function commit(f, value) {
    const edits = writeField(f, value, src);
    if (edits.length) {
      src = applyEdits(src, edits);
      unsaved.push(edits);
      const done = () => { unsaved = unsaved.filter((x) => x !== edits); };
      queue = queue.then(async () => {
        try {
          await persist(edits, f.reloads);
          done();
          if (f.reloads) await readBack();
        } catch (e) {
          done();
          toast(errText(e), 'error');
          await readBack(true).catch((err) => console.error(err));
        }
      });
    }
    draw();
    return queue;
  }

  const later = (id, ms, run) => {
    clearTimeout(pending.get(id)?.timer);
    const entry = { run, timer: setTimeout(() => { pending.delete(id); run(); }, ms) };
    pending.set(id, entry);
  };
  const dropLater = (id) => {
    clearTimeout(pending.get(id)?.timer);
    pending.delete(id);
  };

  /** Everything still waiting goes out; resolves when it is on disk. */
  function flush() {
    for (const [id, p] of [...pending]) {
      dropLater(id);
      p.run();
    }
    return queue;
  }

  function commitNumber(f, t, part, final) {
    const bounds = f.custom ?? f;
    const fit = (n) => fitNumber(n.value, bounds.min, bounds.max);
    const parts = part === 'custom' ? [t] : [el.querySelector(`#${f.id}--w`), el.querySelector(`#${f.id}--h`)];
    const fitted = parts.map(fit);
    const exact = parts.every((n, i) => fitted[i] !== null && Number(n.value) === fitted[i]);
    t.setAttribute('aria-invalid', String(!exact && !final));
    if (!final && !exact) return;              // "6" on the way to "60" is not a value yet
    if (fitted.includes(null)) return draw(true);   // left empty: back to the saved value
    parts.forEach((n, i) => { n.value = fitted[i]; });
    commit(f, fitted.join('x'));
  }

  /** The value a control holds, for the types that need no help. */
  function commitControl(f, t, part, final) {
    if (part) return commitNumber(f, t, part, final);
    if (f.type === 'clock') {
      const picked = new Date(t.value);
      if (!t.value || !Number.isFinite(picked.getTime())) return draw(true);
      return commit(f, Math.round((picked.getTime() - Date.now()) / 60000));
    }
    if (f.type === 'toggle') return commit(f, t.checked);
    if (f.type === 'slider') return commit(f, Number(t.value));
    return commit(f, t.value);
  }

  async function askDlss() {
    return confirm(DLSS_DOWNLOAD);
  }

  /** store.fetchDlss() runs fetch_dlss; the sidebar pill shows it. */
  async function downloadDlss() {
    await store.fetchDlss();
    dlss = await api('dlss_status');
    saved();
    draw();
  }

  async function pickDlss(f, value) {
    const before = readField(f, src);
    if (!(await askDlss())) return draw(true);
    commit(f, value);
    try {
      await downloadDlss();
    } catch (e) {
      toast(errText(e), 'error');
      commit(f, before);   // the upscaler would only run as off without the file
    }
  }

  function onSelect(f, value) {
    if (f.type === 'choice' || f.type === 'size') {
      if (value === 'custom') {
        customOpen.add(f.id);
        const cur = readField(f, src);
        return commit(f, f.type === 'size' ? cur : customStart(f, cur));
      }
      customOpen.delete(f.id);
    }
    if (f.guard === 'dlss' && ['dlss', 'dlaa'].includes(value) && !dlss?.present) return pickDlss(f, value);
    // joining Testing is a question first: what it means, and that a tester is expected to share findings and logs. Cancel keeps Stable.
    if (f.guard === 'testing' && value === 'testing' && readField(f, src) !== 'testing') {
      return askTesting().then(async (yes) => {
        if (!yes) return draw(true);
        await commit(f, value);
        await chooseBuild('testing', true);   // the Testing builds, right away: install one now or stay and update later
      });
    }
    if (f.replaces) customOpen.clear();
    return commit(f, value);
  }

  // ---- buttons ------------------------------------------------------------------------------------------------------------------

  const OPEN = { save: () => api('open_save_dir'), game: () => api('open_game_dir'), backups: () => api('open_backup_dir'), photos: () => api('open_album_dir') };

  async function runImport(what, source, html) {
    if (!(await confirm({ title: `Import ${what}?`, html, body: html ? '' : IMPORT_FACTS, ok: 'Import' }))) return;
    importing = true;
    importResult = null;
    draw();
    try {
      await flush();
      const r = await api('import_save', { source });
      importResult = r.backup
        ? { kind: 'ok', text: `Imported ${r.files} files. Your previous save is kept in`, path: r.backup }
        : { kind: 'ok', text: `Imported ${r.files} files. There was no previous save.` };
      toast('Save imported', 'ok');
    } catch (e) {
      importResult = { kind: 'danger', text: errText(e) };
      toast(errText(e), 'error');
    }
    importing = false;
    draw();
  }

  /** The builds of a channel with their notes; installs the one the player picks, after the question an older build needs. */
  async function chooseBuild(channel, join) {
    const builds = await loadBuilds(channel, true);
    const picked = await pickBuild(join
      ? { title: 'Testing builds', intro: 'You are on the Testing channel now. Pick a build to install, or choose Not now and press Update when you want the newest one.', builds }
      : { title: 'Choose a version', intro: channel === 'testing' ? 'Testing builds and the normal releases. Newer builds install like an update; an older one asks first.' : 'The releases you can install. A newer one installs like an update; an older one asks first.', builds, close: 'Close' });
    if (!picked) return;
    const ok = await confirmBuild(picked);
    if (!ok) return;
    await store.installVersion(picked.tag, channel, ok.accepted);
  }

  async function refreshSteam() {
    try {
      steam = await api('steam_status');
    } catch (e) {
      console.warn('steam_status', e);
    }
    draw();
  }

  /** Adds or removes one Steam shortcut ('game' starts the game at once, 'launcher' opens this window) and says what happened. */
  async function steamDo(cmd, target) {
    if (steamBusy) return;
    steamBusy = true;
    draw();
    try {
      toast(await api(cmd, { target }), 'ok');
    } catch (e) {
      toast(errText(e), 'error');
    }
    steamBusy = false;
    await refreshSteam();
  }

  async function refreshIcloud() {
    try {
      icloud = await api('icloud_status');
    } catch (e) {
      console.warn('icloud_status', e);
    }
    draw();
  }

  async function refreshBackups() {
    try {
      backups = await api('backup_status');
    } catch (e) {
      console.warn('backup_status', e);
    }
    draw();
  }

  async function changeProfiles(cmd, args, done) {
    try {
      await flush();
      profiles = await api(cmd, args);
      await reload();
      toast(done, 'ok');
    } catch (e) {
      toast(errText(e), 'error');
    }
    draw();
  }

  const ACTIONS = {
    async browse(arg) {
      const f = FIELDS[arg];
      const start = readField(f, src) || f.placeholder(env()) || null;
      const picked = await api('pick_folder', { title: f.pick, start });
      if (picked) {
        dropLater(f.id);
        const input = el.querySelector(`#${f.id}`);
        if (input) input.value = picked;
        commit(f, picked);
      }
    },
    async open(arg) {
      await flush();
      await OPEN[arg]();
    },
    async 'dlss-get'() {
      if (await askDlss()) await downloadDlss();
    },
    import: (arg, name) => runImport(`from ${name}`, arg),
    async 'import-folder'() {
      const picked = await api('pick_folder', { title: 'Choose a folder with the save', start: null });
      if (picked) await runImport('this save', picked, `<p class="path selectable">${esc(picked)}</p><p>${esc(IMPORT_FACTS)}</p>`);
    },
    async 'profile-new'() {
      const name = await askText({ title: 'New profile', body: 'It starts with an empty save. Your other saves are not touched.', label: 'Profile name', ok: 'Add' });
      if (name === '') toast('Type a name for the profile', 'error');
      else if (name) await changeProfiles('profile_add', { name }, `Profile ${name} added`);
    },
    'profile-use': (arg, name) => changeProfiles('profile_select', { id: arg }, `Playing as ${name}`),
    async 'profile-rename'(arg, name) {
      const next = await askText({ title: 'Rename profile', label: 'Profile name', value: name, ok: 'Rename' });
      if (next === '') toast('Type a name for the profile', 'error');
      else if (next && next !== name) await changeProfiles('profile_rename', { id: arg, name: next }, 'Profile renamed');
    },
    async 'profile-remove'(arg, name) {
      const yes = await confirm({
        title: `Remove ${name}?`,
        body: 'This only removes the profile from the list. Its save stays in the save folder, so nothing is lost.',
        ok: 'Remove', danger: true,
      });
      if (yes) await changeProfiles('profile_remove', { id: arg }, 'Profile removed');
    },
    'change-files': () => ctx.go('setup'),
    async 'setup-again'() {
      const yes = await confirm({ title: 'Set up again?', body: 'This builds the game and prepares its shaders again, which takes a while. Your saves are kept.', ok: 'Set up' });
      if (yes && (await store.startSetup())) ctx.go('home');
    },
    'setup-log': () => api('open_setup_log'),
    'compact-shaders': () => askCompact(),
    async 'expand-shaders'() {
      const st = store.state.storage;
      const yes = await confirm({
        title: 'Use loose shader files?',
        body: `This writes the ${(st?.archive_shaders ?? 0).toLocaleString('en')} packed shaders back out as separate files, about ${Math.ceil((st?.archive_raw_bytes ?? 0) / 2 ** 30)} GB. The game runs the same either way, and a later update stops reading loose files.`,
        ok: 'Use loose files',
      });
      if (yes) await store.expandShaders();
    },
    'stop-repack': () => store.cancelRepack(),
    async 'repair-open'() {
      await flush();
      const r = await runRepair();
      if (!r) return;
      if (r.launcher) {
        try { localStorage.removeItem('theme'); localStorage.removeItem('pkp.linkplay'); } catch { /* nothing to clear */ }
        location.reload();
        return;
      }
      repairBackup = await api('repair_last').catch(() => null);
      await reload().catch((e) => console.error(e));
      draw();
    },
    async 'repair-undo'() {
      await flush();
      if (await runUndo(repairBackup)) {
        repairBackup = await api('repair_last').catch(() => null);
        await reload().catch((e) => console.error(e));
        toast('Reload the launcher (close and open it) to use the restored launcher settings', 'ok');
      }
      draw();
    },
    async 'check-updates'() {
      checking = true;
      draw();
      await store.refreshNews(true);
      checking = false;
      checked = true;
      draw();
    },
    update: () => store.installUpdate(),
    'clock-real': (id) => commit(FIELDS[id], 0),
    'open-url': (url) => api('open_release_url', { url }),
    async 'builds-open'() {
      await flush();
      await chooseBuild(src.config.channel === 'testing' ? 'testing' : 'stable', false);
    },
    'open-log': () => api('open_game_file', { name: 'pkp.log' }),
    'steam-add-game': () => steamDo('steam_add', 'game'),
    'steam-add-launcher': () => steamDo('steam_add', 'launcher'),
    'steam-remove-game': () => steamDo('steam_remove', 'game'),
    'steam-remove-launcher': () => steamDo('steam_remove', 'launcher'),
    async 'backup-now'() {
      await flush();
      await backupNow();
      await refreshBackups();
    },
    async 'backup-restore'() {
      await flush();
      await runRestore();
      await refreshBackups();
    },
    'backup-open': () => api('open_backup_dir'),
    report: () => reportProblem(),
    'icloud-open': () => api('open_icloud_dir'),
  };

  // ---- the page -----------------------------------------------------------------------------------------------------------------

  el.innerHTML = `<div class="page set">
    <div class="set-head">
      <h1>Settings</h1>
      <span class="set-saved" id="set-saved" role="status"></span>
    </div>
    <div class="set-pills">${segmented('section', SECTIONS.map((s) => ({ value: s.id, label: s.label })), section, { label: 'Settings section' })}</div>
    <div class="set-body" id="set-body"></div>
  </div>`;

  el.addEventListener('change', (e) => {
    const t = e.target;
    if (t.name === 'section') {
      section = lastSection = t.value;
      draw(true);
      el.closest('#view')?.scrollTo(0, 0);
      return;
    }
    const { f, part } = fieldOf(t);
    if (!f) return;
    dropLater(f.id);
    if (t.tagName === 'SELECT') onSelect(f, t.value);
    else commitControl(f, t, part, true);
  });

  el.addEventListener('input', (e) => {
    const t = e.target;
    if (!t.matches('input[type="range"], input.input')) return;
    const { f, part } = fieldOf(t);
    if (!f || f.commit === 'blur') return;
    later(f.id, t.type === 'range' ? SLIDER_WAIT_MS : TEXT_WAIT_MS, () => commitControl(f, t, part, false));
  });

  el.addEventListener('click', async (e) => {
    const b = e.target.closest('[data-act]');
    if (!b) return;
    try {
      await ACTIONS[b.dataset.act](b.dataset.arg, b.dataset.name);
    } catch (err) {
      toast(errText(err), 'error');
    }
  });

  // a pointer press on a row must end on that same row: replacing a button under it loses the click
  const release = () => {
    pointerDown = false;
    setTimeout(() => redraw && draw());
  };
  el.addEventListener('pointerdown', () => { pointerDown = true; });
  window.addEventListener('pointerup', release);
  window.addEventListener('pointercancel', release);
  el.addEventListener('focusout', () => setTimeout(() => redraw && draw()));

  ctx.on((s, why) => {
    const active = env().gameBusy;
    const dlssEnded = hadDlss && !s.dlss;
    hadDlss = !!s.dlss;
    if ((wasActive && !active) || dlssEnded) reload().catch((e) => console.error(e));   // the game writes its settings as it leaves
    else if (why !== 'setup' || section === 'game') draw();
    if (wasActive && !active) {
      refreshIcloud();   // the save went up to iCloud Drive as the game closed
      refreshBackups();
    }
    wasActive = active;
  });
  wasActive = env().gameBusy;

  draw(true);
  store.refreshStorage();
  api('profiles_list').then((list) => {
    profiles = list;
    draw();
  }).catch((e) => console.warn('profiles_list', e));
  api('repair_last').then((info) => {
    repairBackup = info;
    draw();
  }).catch((e) => console.warn('repair_last', e));
  refreshBackups();
  refreshIcloud();
  refreshSteam();
  api('save_sources').then((list) => {
    sources = list;
    draw();
  }).catch((e) => console.warn('save_sources', e));

  return () => {
    window.removeEventListener('pointerup', release);
    window.removeEventListener('pointercancel', release);
    clearTimeout(savedTimer);
    flush();
  };
}
