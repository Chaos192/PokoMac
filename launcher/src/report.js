// Report a problem (launcher/src-tauri/src/report.rs): the logs in a zip on the Desktop, shown in Finder, and a new GitHub issue with the
// system facts filled in. One function, so Settings and the Home page's crash card say the same thing.
import { api, errText, toast } from './ui.js';

let busy = false;

export async function reportProblem() {
  if (busy) return;
  busy = true;
  toast('Collecting the logs...');
  try {
    const r = await api('report_problem');
    toast(r.issue
      ? `Saved ${r.name} ${r.place}. Attach it to the GitHub issue that just opened.`
      : `Saved ${r.name} ${r.place}. Open github.com/Chaos192/PokoMac/issues and attach it to a new issue.`, 'ok', 8000);
  } catch (e) {
    toast(errText(e), 'error');
  } finally {
    busy = false;
  }
}
