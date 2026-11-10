// Home: one button that does the next right thing, what went wrong if anything did, and the newest releases.
import './home.css';
import logoUrl from '../art/logo.png';
import { api, errText, esc, icon, toast } from '../ui.js';
import { store } from '../store.js';
import { canCancelUpdate, canUpdate, gamePhase, primaryAction } from '../action.js';
import { readIni, viewOf } from '../linkplay-model.js';
import { formatDate, highlights, parseReleaseName } from '../markdown.js';
import { reportProblem } from '../report.js';

const REPORT_LABEL = {
  'pkp_crash.txt': 'Crash report',
  'pkp_freeze.txt': 'Freeze report',
  'pkp_fatal.txt': 'Error report',
  'pkp.log': 'Game log',
};
const CARDS = 3;   // home.css shows fewer where they do not fit
const CARD_LINES = 1;   // one highlight, clamped: two half-cut bullets read as broken text

// Game status has no run id, so a dismissed crash is forgotten when the next run starts, on whatever page that happens.
let crashDismissed = false;
store.subscribe((s) => {
  if (s.game?.state !== 'exited') crashDismissed = false;
});

function releaseCard(r) {
  const { version, build } = parseReleaseName(r.name, r.tag);
  const date = formatDate(r.date);
  const lines = highlights(r.body, CARD_LINES);
  return `<button class="release glass hover" data-tag="${esc(r.tag)}">
    <span class="release-head">
      <span class="release-title clip"><strong>${esc(version)}</strong>${build ? ` ${esc(build)}` : ''}${r.prerelease ? ' <span class="note-mark">Testing</span>' : ''}</span>
      ${date ? `<time>${esc(date)}</time>` : ''}
    </span>
    ${lines.map((l) => `<span class="release-line">${esc(l)}</span>`).join('')}
  </button>`;
}

function crashCard(game) {
  const files = (game.files ?? []).filter((f) => f.fresh || f.name === 'pkp.log');
  return `<div class="home-card glass strong" role="alert">
    <div class="home-card-head">
      ${icon('circle-alert')}<h2>The game stopped unexpectedly</h2>
      <button class="btn quiet sm icon-only" data-act="dismiss" aria-label="Dismiss" title="Dismiss">${icon('x')}</button>
    </div>
    <p class="selectable">${esc(game.message)}</p>
    <div class="cluster">
      ${files.map((f) => `<button class="btn sm" data-open="${esc(f.name)}" title="${esc(f.name)}">${icon('file-text')}${esc(REPORT_LABEL[f.name] ?? f.name)}</button>`).join('')}
      <button class="btn sm" data-act="report" title="Puts the logs in a zip on your Desktop and opens a GitHub issue">${icon('file-archive')}Report a problem</button>
    </div>
  </div>`;
}

const updateErrorCard = (s) => `<div class="home-card glass strong" role="alert">
    <div class="home-card-head">${icon('circle-alert')}<h2>The update did not finish</h2></div>
    <p class="selectable">${esc(s.updateError)}</p>
    <div class="cluster"><button class="btn sm" data-act="update" ${canUpdate(s) ? '' : 'disabled'}>${icon('refresh-cw')}Try again</button></div>
  </div>`;

const repackMessage = (r) => {
  const what = r.kind === 'expand' ? 'Unpacking shader files' : 'Packing shader files';
  return r.total ? `${what}: ${r.done.toLocaleString('en')} of ${r.total.toLocaleString('en')}` : `${what}...`;
};

const QUIET = new Set(['ready', 'no_files', 'setup', 'resume']);
const LINK_PLAY_LABEL = { off: 'Off', lan: 'Same network', internet: 'Internet' };

export function render(el, ctx) {
  el.innerHTML = `<div class="page home">
    <div class="home-hero">
      <h1 class="display"><img class="home-logo" src="${logoUrl}" alt="PokoMac" /></h1>
      <div class="home-actions">
        <button class="btn primary big" id="primary"></button>
        <button class="btn big on-art" id="update" data-act="update" hidden>${icon('download')}Update</button>
        <span id="link"></span>
      </div>
      <p class="home-detail" id="detail" hidden></p>
      <div class="home-task glass strong" id="task" hidden>
        <span class="msg clip" id="task-msg"></span>
        <span class="pct num" id="task-pct"></span>
        <progress max="100" id="task-bar"></progress>
        <button class="btn sm" id="task-stop"></button>
      </div>
      <div class="home-notices" id="notices"></div>
    </div>
    <section class="home-news" id="news" aria-label="Newest releases"></section>
  </div>`;

  const part = (id) => el.querySelector(`#${id}`);
  // events arrive many times a second: a part is rewritten only when it changed, so focus and hover stay
  const patch = (node, html) => {
    if (node.dataset.html !== html) {
      node.innerHTML = html;
      node.dataset.html = html;
    }
  };

  function drawTask(s) {
    const box = part('task');
    const kind = s.task?.kind;
    box.hidden = !['update', 'setup', 'repack'].includes(kind);
    if (box.hidden) return;
    const update = kind === 'update';
    const msg = update ? s.task.label
      : kind === 'repack' ? repackMessage(s.repack)
      : s.setup.progress.message || s.task.label;
    const stop = part('task-stop');
    part('task-msg').textContent = msg;
    part('task-msg').title = msg;
    part('task-pct').textContent = s.task.pct === null ? '' : `${s.task.pct}%`;
    if (s.task.pct === null) part('task-bar').removeAttribute('value');
    else part('task-bar').value = s.task.pct;
    stop.dataset.act = update ? 'cancel' : kind === 'repack' ? 'stop-repack' : 'pause';
    patch(stop, update ? `${icon('x')}Cancel` : kind === 'repack' ? `${icon('square')}Stop` : `${icon('pause')}Pause`);
    stop.hidden = update && !canCancelUpdate(s);
  }

  function draw(s) {
    const a = primaryAction(s);
    const button = part('primary');
    patch(button, `${icon(a.icon, a.busy ? 'spin' : '')}${esc(a.label)}`);
    button.disabled = a.disabled;
    button.setAttribute('aria-busy', a.busy);

    const update = part('update');
    update.hidden = !canUpdate(s);
    update.title = s.news.update ? `Update to ${parseReleaseName(s.news.update.name, s.news.update.tag).version}` : '';

    const ready = s.app?.readiness;
    // the button already says what no_files/setup/resume need; the detail only explains a problem
    const showDetail = ready && !QUIET.has(ready.kind) && !a.busy && gamePhase(s.game) !== 'running';
    const detail = part('detail');
    detail.textContent = showDetail ? ready.detail : s.app ? '' : s.loadError ?? '';
    detail.title = detail.textContent;
    detail.hidden = !detail.textContent;

    drawTask(s);

    const crashed = s.game?.state === 'exited' && s.game.crashed && !crashDismissed;
    patch(part('notices'), (s.updateError ? updateErrorCard(s) : '') + (crashed ? crashCard(s.game) : ''));
    el.firstElementChild.classList.toggle('noticed', !!(crashed || s.updateError));

    patch(part('news'), s.news.releases.slice(0, CARDS).map(releaseCard).join(''));
  }

  el.addEventListener('click', (e) => {
    const t = e.target.closest('button');
    if (!t || t.disabled) return;
    if (t.id === 'primary') store.perform(primaryAction(store.state).run, ctx.go);
    if (t.id === 'link-chip') ctx.go('linkplay');
    if (t.dataset.tag) ctx.go('news', { tag: t.dataset.tag });
    if (t.dataset.open) api('open_game_file', { name: t.dataset.open }).catch((err) => toast(errText(err), 'error'));
    switch (t.dataset.act) {
      case 'update': return store.installUpdate();
      case 'cancel': return store.cancelUpdate();
      case 'pause': return store.pauseSetup();
      case 'stop-repack': return store.cancelRepack();
      case 'report': return reportProblem();
      case 'dismiss':
        crashDismissed = true;
        return draw(store.state);
    }
  });

  // a settings file that cannot be read just leaves the chip out
  let linkChip = '';
  let profileChip = '';
  const drawChips = () => { part('link').innerHTML = profileChip + linkChip; };
  drawChips();
  api('read_settings').then((data) => {
    linkChip = `<button class="chip on-art" id="link-chip">${icon('users')}Link Play: ${LINK_PLAY_LABEL[viewOf(readIni(data.pkp))]}</button>`;
    drawChips();
  }).catch((e) => console.warn('read_settings', e));
  // with more than one profile the player picks who plays here; the choice is the Settings > Saves list
  const profileSelect = (list, busy) => `<select class="chip on-art" id="profile-pick" aria-label="Profile" ${busy ? 'disabled' : ''}>${
    list.profiles.map((p) => `<option value="${esc(p.id)}" ${p.id === list.active ? 'selected' : ''}>Profile: ${esc(p.name)}</option>`).join('')}</select>`;
  const loadProfiles = () => api('profiles_list').then((list) => {
    profileChip = list.profiles.length > 1 ? profileSelect(list, ['starting', 'running', 'finishing'].includes(gamePhase(store.state.game))) : '';
    drawChips();
  }).catch((e) => console.warn('profiles_list', e));
  loadProfiles();
  el.addEventListener('change', async (e) => {
    if (e.target.id !== 'profile-pick') return;
    try {
      await api('profile_select', { id: e.target.value });
    } catch (err) {
      toast(errText(err), 'error');
    }
    loadProfiles();
  });

  ctx.on(draw);
  draw(store.state);
}
