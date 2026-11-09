// Dev only: every shared control on one page, /?page=kit, for reviewing the design system.
import { confirm, esc, field, group, icon, row, segmented, select, slider, toast, toggle } from '../ui.js';

export function render(el) {
  el.innerHTML = `<div class="page">
    <h1>Kit</h1>
    ${segmented('section', ['Display', 'Graphics', 'Audio', 'Controls', 'Saves', 'Game', 'About'], 'Graphics', { label: 'Section' })}
    ${group('Display', [
      row('Window mode', select('window', [['windowed', 'Windowed'], ['borderless', 'Borderless fullscreen'], ['exclusive', 'Exclusive fullscreen']], 'borderless')),
      row('Frame rate limit', segmented('fps', [['auto', 'Match display'], ['60', '60'], ['0', 'Unlimited'], ['custom', 'Custom']], '60')),
      row('VSync', toggle('vsync', true)),
      row('Keep 16:9', toggle('aspect', false)),
      row('Master volume', slider('vol', 0, 100, 80, { suffix: '%' })),
      row('Room code', field('code', '', 'XXXX-XXXX-XXXX-XXXX', { mono: true })),
      row('Disabled', toggle('off', true, { disabled: true })),
      row('A long row label that has to share its line with a wide control beside it', segmented('w', ['Off', 'Same network', 'Internet'], 'Off')),
    ])}
    <div class="cluster">
      <button class="btn primary">${icon('play')}Primary</button>
      <button class="btn">${icon('folder-open')}Default</button>
      <button class="btn quiet">Quiet</button>
      <button class="btn danger">Danger</button>
      <button class="btn" disabled>Disabled</button>
      <button class="btn primary" disabled>${icon('loader-circle', 'spin')}Busy</button>
      <button class="btn sm">Small</button>
    </div>
    <div class="cluster">
      <span class="chip">Plain</span><span class="chip ok"><span class="dot"></span>Ready</span><span class="chip warn">${icon('triangle-alert')}Warn</span><span class="chip danger">${icon('circle-alert')}Danger</span>
    </div>
    <div class="banner info">${icon('info')}<div class="grow">Changes apply next time the game starts</div></div>
    <div class="banner ok">${icon('circle-check')}<div class="grow">Ok banner</div></div>
    <div class="banner warn">${icon('triangle-alert')}<div class="grow">Warning banner</div></div>
    <div class="banner danger">${icon('circle-x')}<div class="grow">Danger banner</div></div>
    <div class="card glass">
      <h2>Card</h2>
      <dl class="kv"><dt>Game folder</dt><dd class="path">C:/Games/PokoMac</dd><dt>Game files</dt><dd>Update 2.0.1a</dd></dl>
    </div>
    <progress max="100" value="42"></progress>
    <div class="empty glass">${icon('newspaper')}<strong>Nothing here yet</strong><span>An empty state</span></div>
    <div class="cluster">
      <button class="btn" id="ask">Dialog</button>
      <button class="btn" id="ask-danger">Danger dialog</button>
      <button class="btn" id="say">Toasts</button>
    </div>
  </div>`;
  el.querySelector('#ask').addEventListener('click', async () => toast(`Answer: ${esc(await confirm({ title: 'Download DLSS?', body: 'By downloading it you accept the NVIDIA RTX SDKs licence.', ok: 'Download' }))}`));
  el.querySelector('#ask-danger').addEventListener('click', () => confirm({ title: 'Replace your save?', body: 'The current save is backed up first.', ok: 'Replace', danger: true }));
  el.querySelector('#say').addEventListener('click', () => { toast('Saved'); toast('Setup finished', 'ok'); toast('The game files could not be read.', 'error'); });
}
