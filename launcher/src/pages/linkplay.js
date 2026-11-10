// Link Play. What the settings mean, and every edit, is in ../linkplay-model.js; this page is the fields and the calls around them. Nothing is
// written on opening. A choice or a field applies itself: a commit reads the whole page, so nothing half-written reaches pkp.ini.
// Fields never redraw on commit (tabbing to the next field must keep its focus); only a mode, role or toggle change redraws, and that keeps focus.
import './linkplay.css';
import { api, errText, esc, field, group, icon, row, segmented, toast, toggle } from '../ui.js';
import { store } from '../store.js';
import { gamePhase } from '../action.js';
import {
  HOST_PORT, RELAY_PORT, addrProblem, addressRows, applyEdits, codeEdits, editsFor, hostsOverInternet, interactEdits, internetShare, pathParts,
  protocolEdits, readIni, relayPort, shownCode, storyEdits, uiFrom,
} from '../linkplay-model.js';

const LS = 'pkp.linkplay';
const POLL_MS = 3000;   // link count and the game's room code, while the Host view is on screen

const stored = () => { try { return JSON.parse(localStorage.getItem(LS)) || {}; } catch { return {}; } };
const keep = (patch) => { try { localStorage.setItem(LS, JSON.stringify({ ...stored(), ...patch })); } catch { /* private window: fine */ } };

const gameActive = (s) => ['starting', 'running', 'finishing'].includes(gamePhase(s.game));

// which `ui` value each field edits
const FIELDS = { 'lp-join': 'lanJoin', 'lp-relay': 'relay', 'lp-addr': 'visitAddr', 'lp-code-in': 'visitCode' };

export async function render(el, ctx) {
  let pkp = { ...(await api('read_settings')).pkp };
  const mem = stored();
  const ui = uiFrom(pkp, mem);
  let relay = await api('relay_status').catch(() => null);
  // this PC's relay, remembered from last time, is only offered while it runs: the launcher just started, so it does not until the
  // toggle is turned on, and a host there would open the room on a relay that is not there (#239)
  if (!relay && /^127\./.test(ui.relay.trim())) ui.relay = '';
  let lan = null;
  let publicIp;            // this PC's public address: undefined not asked yet, null not found
  let asking = false;
  let gameCode = null;     // room-code.txt: the game's own
  let dropped = null;      // the game's code the player replaced with New code
  let ports = null;        // port_forward_status (portmap.rs): automatic port forwarding on or off, and how the router answered
  let problems = {};       // field id -> what is wrong with it

  // commits run one at a time, in the order the player made them: a blur, then the click that caused it
  let tail = Promise.resolve();
  const inOrder = (fn) => { tail = tail.then(fn).catch((e) => toast(errText(e), 'error')); return tail; };

  const remember = (patch) => { Object.assign(mem, patch); keep(patch); };
  const isHost = () => ui.view === 'internet' && ui.role === 'host';
  const isVisit = () => ui.view === 'internet' && ui.role === 'visit';

  async function save(edits) {
    await api('write_settings', { edits });
    pkp = applyEdits(pkp, edits);
    ui.legacy = readIni(pkp).legacy;
  }

  async function loadLan() {
    lan = await api('lan_report').catch(() => null);
    drawLan();
  }

  // asked only for an internet host: one HTTPS request to a what-is-my-IP service (launcher/src-tauri/src/public_ip.rs)
  async function loadPublicIp() {
    if (publicIp || asking) return;
    asking = true;
    publicIp = await api('public_ip').catch(() => null) ?? null;
    asking = false;
    patch(part('lp-share'), shareHtml());
  }

  async function loadPorts() {
    ports = await api('port_forward_status').catch(() => null);
    patch(part('lp-share'), shareHtml());
  }

  async function loadGameCode() {
    const c = await api('room_code_host').catch(() => null);
    gameCode = c === dropped ? null : c;
    drawCode();
  }

  // --- what is on the page

  const copyButton = (addr) => `<button class="btn quiet sm lp-copy" data-copy="${esc(addr)}" aria-label="Copy ${esc(addr)}" title="Copy">${icon('copy')}</button>`;
  const addressRow = (r) => row(r.label, `<code class="selectable">${esc(r.addr)}</code>${copyButton(r.addr)}`);
  const fieldAt = (id, placeholder) => `<div class="lp-field">${field(id, ui[FIELDS[id]], placeholder, { mono: true })}<small class="lp-msg" id="${id}-msg" hidden></small></div>`;
  const visitPlaceholder = () => (ui.through ? `203.0.113.5:${RELAY_PORT}` : '192.0.2.10');

  function bannersHtml() {
    const legacy = ui.legacy ? `<div class="banner warn">${icon('triangle-alert')}<div class="grow">This install uses the old Link Play protocol</div><button class="btn sm" data-act="switch">Switch to current</button></div>` : '';
    const later = gameActive(store.state) ? `<div class="banner info">${icon('info')}<div class="grow">Changes apply next time the game starts</div></div>` : '';
    return legacy + later;
  }

  const live = () => ui.view !== 'off' || ui.legacy;   // the old protocol ignores mode, so Off does not stop it

  function pathHtml() {
    if (!live()) return '';
    // the menu path inside the game (its Link Play menu always says Local connection), not a breadcrumb of this page
    return `<span class="lp-path-lead">${icon('gamepad-2')}In game</span>`
      + pathParts(ui).map((p) => `<span class="chip on-art">${esc(p)}</span>`).join(icon('chevron-right'));
  }

  function sharedHtml() {
    if (!live()) return '';
    const exp = '<span class="chip lp-exp">Experimental</span>';
    return group('Same for everyone', [
      row('Visitors can build everywhere', exp + toggle('lp-interact', ui.interact || ui.story, { disabled: ui.story })),
      row('Visitors can talk to story characters', exp + toggle('lp-story', ui.story)),
    ]);
  }

  function lanHtml() {
    if (!lan) return '';
    const notice = [lan.warning, lan.firewall_hint].filter(Boolean);
    const note = notice.length ? `<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(notice[0])}${notice[1] ? `<div>${esc(notice[1])}</div>` : ''}</div></div>` : '';
    const rows = addressRows(lan, HOST_PORT).map(addressRow);
    return note + group('This PC', rows.length ? rows : [row('No address found', '')]);
  }

  const banner = (kind, ic, text) => `<div class="banner ${kind}">${icon(ic)}<div class="grow">${esc(text)}</div></div>`;

  // the router port for this PC's game or relay: opened by the launcher (portmap.rs) unless the player switched that off
  function routerHtml(share, map) {
    if (!ports?.enabled) return banner('info', 'info', `Your router must forward port ${share.port} (TCP and UDP) to this PC.`);
    if (!map) {
      return banner('info', 'info', share.through
        ? `While the relay runs, the launcher asks your router to open port ${share.port} (TCP and UDP) for you.`
        : `When the game starts, the launcher asks your router to open port ${share.port} (TCP and UDP) for you.`);
    }
    if (map.state === 'trying') return banner('info', 'loader-circle', map.message);
    if (map.state === 'open' && !map.unreachable) return banner('ok', 'circle-check', map.message);
    return banner('warn', 'triangle-alert', map.message);
  }

  // what visitors type: the public address over the internet (#245), this PC's LAN addresses on the same network
  function shareHtml() {
    if (!isHost()) return '';
    const share = internetShare(ui, { publicIp, relayOn: relay ? relayPort(relay) : 0 });
    if (!share) {
      return `<div class="banner warn">${icon('triangle-alert')}<div class="grow">The relay on this PC is off, so visitors cannot join. Turn on Run a relay on this PC.</div></div>`;
    }
    // the router's own answer is the address visitors reach, when it gave one that is not private
    const map = share.own ? ports?.maps.find((m) => m.port === share.port) : null;
    const mapped = map?.state === 'open' && !map.unreachable ? map.external : '';
    const internet = mapped || share.addr
      ? addressRow({ addr: mapped || share.addr, label: 'Over the internet' })
      : row('Over the internet', `<span class="faint">${publicIp === null ? 'Could not find this PC\'s public address. Search "what is my IP" in a browser.' : 'Looking it up\u2026'}</span>`);
    // a VPN (Tailscale and the like) keeps its own name: it is not the same network
    const local = share.own && relay && lan
      ? [...lan.lan.map((a) => addressRow({ addr: `${a.ip}:${share.port}`, label: `Same network (${a.adapter})` })),
        ...addressRows({ lan: [], overlay: lan.overlay }, share.port).map(addressRow)]
      : [];
    const links = relay ? [row('Links', `<span class="num" id="lp-links">${esc(relay.clients)}</span>`)] : [];
    const auto = share.own ? [row('Open the router port automatically', toggle('lp-portmap', ports ? ports.enabled : true, { disabled: !ports }))] : [];
    const how = `Visitors pick Visit and type this address and the room code, with Through a relay ${share.through ? 'on' : 'off'}.`;
    return group('Share with visitors', [internet, ...local, ...links, ...auto])
      + (share.own ? routerHtml(share, map) : '')
      + `<div class="banner info">${icon('info')}<div class="grow">${how} Never give out 127.0.0.1: on a visitor's PC it means their own PC.</div></div>`;
  }

  function bodyHtml() {
    if (ui.view === 'off') return '';
    if (ui.view === 'lan') {
      return `<div class="lp-part" id="lp-lan"></div>
        ${group('', [row('Join directly', fieldAt('lp-join', '192.0.2.10'))])}
        ${ui.lastJoin ? `<datalist id="lp-last"><option value="${esc(ui.lastJoin)}"></option></datalist>` : ''}`;
    }
    const role = segmented('role', [['host', 'Host'], ['visit', 'Visit']], ui.role, { label: 'Host or visit' });
    if (ui.role === 'host') {
      return `<div>${role}</div>
        ${group('', [
          row('Room code', `<code class="selectable" id="lp-code"></code>${copyButton('')}<button class="btn sm" data-act="new-code">${icon('refresh-cw')}New code</button>`),
          row('Relay address', fieldAt('lp-relay', 'No relay')),
          row('Run a relay on this PC', toggle('lp-run-relay', !!relay)),
        ])}
        <div class="lp-part" id="lp-share"></div>`;
    }
    return `<div>${role}</div>
      ${group('', [
        row('Address', fieldAt('lp-addr', visitPlaceholder())),
        row('Room code', fieldAt('lp-code-in', 'XXXX-XXXX-XXXX-XXXX')),
        row('Through a relay', toggle('lp-through', ui.through)),
      ])}`;
  }

  // a part is only rewritten when it changed; focus survives a rewrite
  function patch(node, html) {
    if (!node || node.dataset.html === html) return false;
    const a = document.activeElement;
    const key = node.contains(a) ? (a.id ? `#${a.id}` : a.name ? `input[name="${a.name}"]:checked` : null) : null;
    node.innerHTML = html;
    node.dataset.html = html;
    if (key) node.querySelector(key)?.focus();
    return true;
  }

  const part = (id) => el.querySelector(`#${id}`);

  function drawLan() {
    patch(part('lp-lan'), lanHtml());
    patch(part('lp-share'), shareHtml());
  }

  function drawCode() {
    const code = part('lp-code');
    if (!code) return;
    const shown = shownCode({ gameCode, hostCode: ui.hostCode, running: gameActive(store.state) });
    code.textContent = shown || 'New each time the game starts';
    code.classList.toggle('faint', !shown);
    const button = code.nextElementSibling;
    button.dataset.copy = shown;
    button.disabled = !shown;
    button.setAttribute('aria-label', 'Copy room code');
  }

  function drawProblems() {
    for (const id of Object.keys(FIELDS)) {
      const input = part(id);
      if (!input) continue;
      const text = problems[id] || '';
      const msg = part(`${id}-msg`);
      msg.textContent = text;
      msg.hidden = !text;
      if (text) {
        input.setAttribute('aria-invalid', 'true');
        input.setAttribute('aria-describedby', msg.id);
      } else {
        input.removeAttribute('aria-invalid');
        input.removeAttribute('aria-describedby');
      }
    }
  }

  function drawChrome() {
    patch(part('lp-banners'), bannersHtml());
    patch(part('lp-path'), pathHtml());
  }

  function draw() {
    drawChrome();
    patch(part('lp-shared'), sharedHtml());
    if (patch(part('lp-body'), bodyHtml())) {
      part('lp-join')?.setAttribute('list', 'lp-last');
      drawLan();
      drawCode();
    }
    drawProblems();
  }

  // --- commits

  function checkFormat() {
    if (ui.view === 'lan') problems['lp-join'] = addrProblem(ui.lanJoin, { lan: true, remote: true });
    if (isHost()) problems['lp-relay'] = addrProblem(ui.relay, { port: RELAY_PORT });
    if (isVisit()) problems['lp-addr'] = addrProblem(ui.visitAddr, { port: ui.through ? RELAY_PORT : HOST_PORT, remote: true });
  }

  /** Applies the page as it stands, if it is complete. `left`: the field just left, so an empty one beside a filled one is called out. */
  async function commit(left) {
    checkFormat();
    if (isVisit()) {
      const code = ui.visitCode.trim();
      problems['lp-code-in'] = '';
      if (code) {
        try {
          ui.visitCode = await api('room_code_check', { code });
          const input = part('lp-code-in');
          if (input) input.value = ui.visitCode;
        } catch (e) { problems['lp-code-in'] = errText(e); }
      }
      if (left === 'lp-addr' && !ui.visitAddr.trim() && code) problems['lp-addr'] = 'Needed';
      if (left === 'lp-code-in' && !code && ui.visitAddr.trim()) problems['lp-code-in'] = 'Needed';
    }
    drawProblems();
    if (Object.values(problems).some(Boolean)) return;

    let edits = editsFor(ui, pkp);
    if (!edits?.length) return;
    if (isHost() && !ui.hostCode) {
      ui.hostCode = mem.hostCode || await api('room_code_new');
      edits = editsFor(ui, pkp);
    }
    await save(edits);
    if (ui.view === 'lan' && ui.lanJoin.trim()) remember({ joinAddr: ui.lanJoin.trim() });
    if (isHost()) remember({ relayAddr: ui.relay.trim(), hostCode: ui.hostCode });
    if (isVisit()) {
      const addr = ui.visitAddr.trim();
      remember(ui.through ? { relayAddr: addr, visitCode: ui.visitCode } : { joinAddr: addr, joinCode: ui.visitCode });
      if (ui.through) ui.relay = addr;   // one relay for both roles: Host shows what Visit just set
    }
    drawChrome();
    drawCode();
    patch(part('lp-share'), shareHtml());   // the relay field decides what visitors are given
  }

  function enter() {
    if (ui.view === 'lan' && !lan) loadLan();
    if (isHost()) {
      loadGameCode();
      loadPublicIp();
      loadPorts();
      if (relay && !lan) loadLan();
    }
  }

  const changeView = (view) => inOrder(async () => {
    const was = ui.view;
    ui.view = view;
    problems = {};
    draw();
    enter();
    try { await commit(); } catch (e) {
      ui.view = was;
      el.querySelector(`input[name="view"][value="${was}"]`).checked = true;
      draw();
      throw e;
    }
  });

  const changeRole = (role) => inOrder(async () => {
    ui.role = role;
    remember({ role });
    if (role === 'visit' && ui.through && ui.relay.trim()) ui.visitAddr = ui.relay.trim();
    problems = {};
    draw();
    enter();
    await commit();
  });

  const setRelay = (on) => inOrder(async () => {
    try {
      if (on) {
        relay = await api('relay_start', { port: RELAY_PORT });
        await loadLan();
        loadPorts();
        if (!ui.relay.trim()) ui.relay = `127.0.0.1:${relayPort(relay)}`;
        await commit();   // also when the field was filled: the file may still say join:<ip>, and the game could not host (#239)
      } else {
        const own = `127.0.0.1:${relayPort(relay)}`;   // left pointing at the relay just stopped, the game could not host
        await api('relay_stop');
        relay = null;
        loadPorts();
        if (ui.relay.trim() === own) {
          ui.relay = '';
          await commit();
        }
      }
    } finally {
      part('lp-body').dataset.html = '';   // the toggle and the field have moved on from the drawn markup
      draw();
    }
  });

  const newCode = () => inOrder(async () => {
    const code = await api('room_code_new');
    ui.hostCode = code;
    remember({ hostCode: code });
    dropped = gameCode;
    gameCode = null;
    if (hostsOverInternet(readIni(pkp))) await save(codeEdits(code));
    drawChrome();
    drawCode();
  });

  const setPorts = (on, input) => inOrder(async () => {
    try {
      ports = await api('port_forward_set', { enabled: on });
    } catch (e) {
      input.checked = !on;
      throw e;
    }
    patch(part('lp-share'), shareHtml());
  });

  const setShared = (edits, input) => inOrder(async () => {
    try {
      await save(edits);
      const n = readIni(pkp);
      ui.story = n.story;
      ui.interact = n.interact;
    } catch (e) {
      input.checked = !input.checked;
      throw e;
    }
    patch(part('lp-shared'), sharedHtml());
  });

  async function copy(text) {
    try {
      await navigator.clipboard.writeText(text);
      toast('Copied.', 'ok');
    } catch { toast('Cannot copy here. Select the text and press Ctrl+C.', 'error'); }
  }

  el.innerHTML = `<div class="page linkplay">
    <div class="lp-head">
      <h1>Link Play</h1>
      ${segmented('view', [['off', 'Off'], ['lan', 'Same network'], ['internet', 'Internet']], ui.view, { label: 'Link Play' })}
    </div>
    <div class="lp-part" id="lp-banners"></div>
    <div class="lp-path" id="lp-path"></div>
    <div class="lp-part" id="lp-body"></div>
    <div class="lp-part" id="lp-shared"></div>
  </div>`;

  el.addEventListener('input', (e) => {
    const key = FIELDS[e.target.id];
    if (!key) return;
    ui[key] = e.target.value;
    if (problems[e.target.id]) {
      problems[e.target.id] = '';
      drawProblems();
    }
  });
  el.addEventListener('focusout', (e) => {
    if (FIELDS[e.target.id]) inOrder(() => commit(e.target.id));
  });
  el.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && FIELDS[e.target.id]) inOrder(() => commit(e.target.id));
  });
  el.addEventListener('change', (e) => {
    const t = e.target;
    if (t.name === 'view') changeView(t.value);
    else if (t.name === 'role') changeRole(t.value);
    else if (t.id === 'lp-run-relay') setRelay(t.checked);
    else if (t.id === 'lp-portmap') setPorts(t.checked, t);
    else if (t.id === 'lp-interact') setShared(interactEdits(t.checked), t);
    else if (t.id === 'lp-story') setShared(storyEdits(t.checked), t);
    else if (t.id === 'lp-through') {
      ui.through = t.checked;
      remember({ through: t.checked });
      part('lp-addr').placeholder = visitPlaceholder();
      inOrder(() => commit());
    }
  });
  el.addEventListener('click', (e) => {
    const t = e.target.closest('[data-copy], [data-act]');
    if (!t) return;
    if (t.dataset.copy) copy(t.dataset.copy);
    else if (t.dataset.act === 'new-code') newCode();
    else if (t.dataset.act === 'switch') inOrder(async () => { await save(protocolEdits()); draw(); });
  });

  ctx.on((s, why) => {
    drawChrome();
    if (why === 'game' && isHost()) loadGameCode();
  });

  const timer = setInterval(async () => {
    if (document.hidden || !isHost()) return;
    const now = await api('relay_status').catch(() => relay);
    const flipped = !!now !== !!relay;
    relay = now;
    if (flipped) {
      part('lp-body').dataset.html = '';
      draw();
    } else if (relay) {
      const links = part('lp-links');
      if (links) links.textContent = relay.clients;
    }
    loadGameCode();
    loadPorts();
  }, POLL_MS);

  checkFormat();
  draw();
  enter();
  return () => clearInterval(timer);
}
