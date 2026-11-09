// The question asked when someone switches the launcher to the Testing channel: what it means, what is expected of a tester, and how to
// report. Resolves true for "I understand, join Testing"; Cancel, Esc or a click outside give false and the channel stays as it was.
import { confirm } from '../ui.js';

export const TESTING_NOTICE_HTML = `
  <p>Testing builds are newer and less tested than the normal releases. They can have bugs, crashes or things that stop working.</p>
  <p><strong>By joining you are helping us test, so please tell us what you find.</strong> Use <em>Settings &gt; About &gt; Report a problem</em>, say you are on Testing and which version you have, and attach <code>pkp.log</code> from the game folder (the <em>Show pkp.log</em> button opens it), plus <code>setup.log</code> if Set up ran.</p>
  <p>Each Testing build lists what to test and its known problems in <em>What's New</em>. Your saves, profiles and settings are kept; if you want to be safe, copy your save folder first.</p>
  <p>The launcher only moves forward: to go back to Stable you wait for a Stable version newer than the one you have.</p>`;

export const askTesting = () => confirm({ title: 'Join the Testing channel?', html: TESTING_NOTICE_HTML, ok: 'I understand, join Testing', cancel: 'Cancel' });
