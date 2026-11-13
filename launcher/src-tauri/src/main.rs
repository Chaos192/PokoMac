// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod cheats_fetch;
mod cheats_themes;
mod dlss;
mod dump;
mod events;
mod gamebuild;
mod host;
mod icloud;
mod ini;
mod lan;
mod mods;
mod nsp;
mod nup;
mod play;
mod portmap;
mod profiles;
mod preflight;
mod public_ip;
mod relay;
mod repair;
mod report;
mod rollback;
mod saveimport;
mod savebackup;
mod settings;
mod setuplog;
mod shader_archive;
mod shaders;
mod steam;
mod update;
mod writecheck;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

/// This run was started by a self-update (--updated). launcher_state tells the page once.
static UPDATED: AtomicBool = AtomicBool::new(false);

/// The launcher's own options, stored in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Config {
    /// Root of the decrypted dump (the folder with base/ and updates/).
    dump_dir: String,
    /// Folder with pkp.exe. Empty: the folder the launcher is in (macOS: default_game_dir, outside the app).
    game_dir: String,
    minimize_on_play: bool,
    /// Set up with fewer cores at idle priority (preflight.rs jobs_for)
    gentle_build: bool,
    /// A new setup keeps its translated shaders as ~99,000 loose files (the old storage) instead of the shader archive. Only read when
    /// a setup starts with no translated shader yet (shaders.rs Job::compact). Loose storage is on its way out.
    loose_shaders: bool,
    /// Where the window was last time, on a desktop screen (logical pixels); none: the default size, centred
    window: Option<WindowPlace>,
    /// Which releases the launcher offers: "stable" (the default) or "testing" (prereleases built from the testing branch). update.rs Channel.
    channel: String,
    /// Keep my saves in iCloud Drive (macOS, icloud.rs): the save is copied there when the game closes and a newer one is brought in
    /// before it starts. Off until the player switches it on.
    icloud_sync: bool,
    /// When the save was last in sync with iCloud Drive (UTC, ISO 8601), and the hash (savebackup.rs digest) each account's save had
    /// then. Written by icloud.rs, which reads and writes them as JSON so `--play` can too.
    icloud_last_sync: String,
    icloud_synced: std::collections::BTreeMap<String, String>,
    /// The player switched off opening the Link Play port on the router (portmap.rs); forwarding is on by default.
    port_forward_off: bool,
    /// Fetch the game's public cheat file (cheats_fetch.rs); absent: on.
    #[serde(skip_serializing_if = "Option::is_none")]
    online_cheats: Option<bool>,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
struct WindowPlace {
    x: f64,
    y: f64,
    width: f64,
    height: f64,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

/// Tauri's identifier (tauri.conf.json), the name of the launcher's config folder.
const IDENTIFIER: &str = "io.github.chaos192.pokomac";

/// The config file config_path finds through the window, for `--play`, which has none: Tauri's app_config_dir is
/// `<config folder>/<identifier>`, the config folder being ~/Library/Application Support on macOS, %APPDATA% on Windows and
/// $XDG_CONFIG_HOME (~/.config) on Linux. None when the home folder is not known.
fn cli_config_path() -> Option<PathBuf> {
    let env = |k: &str| std::env::var_os(k).filter(|v| !v.is_empty()).map(PathBuf::from);
    #[cfg(target_os = "macos")]
    let base = env("HOME").map(|h| h.join("Library").join("Application Support"));
    #[cfg(windows)]
    let base = env("APPDATA");
    #[cfg(not(any(windows, target_os = "macos")))]
    let base = env("XDG_CONFIG_HOME").or_else(|| env("HOME").map(|h| h.join(".config")));
    Some(base?.join(IDENTIFIER).join("launcher.json"))
}

#[cfg(test)]
mod config_tests {
    #[test]
    fn the_identifier_is_tauris() {
        assert!(include_str!("../tauri.conf.json").contains(&format!("\"identifier\": \"{}\"", super::IDENTIFIER)));
    }
}

fn load_config(app: &AppHandle) -> Config {
    config_path(app)
        .ok()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

/// The launcher's folder, where `tools/` is. An AppImage runs from a temporary mount: there it is the folder of the
/// .AppImage file itself ($APPIMAGE, set by the AppImage runtime).
fn exe_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("APPIMAGE").map(PathBuf::from).and_then(|p| p.parent().map(Path::to_path_buf)) {
        return dir;
    }
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

/// The release's `tools/`: next to the launcher. macOS: inside the app, `PokoMac.app/Contents/Resources/tools` (read only;
/// what Set up downloads goes to host::native_tools); a build that is not in a bundle (cargo run) keeps it next to itself.
fn tools_dir() -> PathBuf {
    #[cfg(target_os = "macos")]
    if let Some(app) = host::bundle_of(&exe_dir()) {
        return app.join("Contents").join("Resources").join("tools");
    }
    exe_dir().join("tools")
}

/// The game folder when Settings names none: the launcher's own folder. macOS: `~/Library/Application Support/PokoMac`
/// (host::support_dir), since the app bundle must stay as it was signed; an unbundled build keeps its own folder.
fn default_game_dir() -> PathBuf {
    #[cfg(target_os = "macos")]
    if host::bundle_of(&exe_dir()).is_some() {
        // unlike the launcher's own folder elsewhere, it may not exist yet (a first start): pkp.ini and setup.log go straight into it
        let dir = host::support_dir();
        let _ = fs::create_dir_all(&dir);
        return dir;
    }
    exe_dir()
}

/// The folder an update installs into: the launcher's. macOS: the folder holding the app, which update.rs replaces as a whole.
fn install_dir() -> PathBuf {
    #[cfg(target_os = "macos")]
    if let Some(parent) = host::bundle_of(&exe_dir()).and_then(|app| app.parent().map(Path::to_path_buf)) {
        return parent;
    }
    exe_dir()
}

fn game_dir_of(cfg: &Config) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        default_game_dir()
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

#[derive(Serialize)]
struct Overview {
    game_dir: String,
    default_game_dir: String,
    exe_exists: bool,
    /// this release builds pkp.exe itself (Set up), so a missing one only means setup has not run yet
    builds_exe: bool,
}

#[tauri::command]
fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    settings::write_atomic(&config_path(&app)?, &text).map_err(|e| format!("cannot save launcher settings: {}", e))
}

/// The version updates compare with. None: a dev build, which takes none.
fn update_version(app: &AppHandle) -> Option<String> {
    #[allow(unused_mut)]
    let mut version = app.package_info().version.to_string();
    #[cfg(debug_assertions)]
    if let Ok(v) = std::env::var("PKP_LAUNCHER_UPDATE_CHECK_AS") {
        version = v; // debug builds only: pretend to be this version (dev builds otherwise skip the check)
    }
    (!update::is_dev_version(&version)).then_some(version)
}

/// The releases (update.rs load_releases) from this session's copy, the network or the cache file.
fn releases_of(app: &AppHandle, refresh: bool) -> Vec<update::GhRelease> {
    let cache = config_path(app).map(|p| p.with_file_name("releases.json")).unwrap_or_default();
    update::load_releases(&app.state::<update::ReleaseStore>(), &cache, &app.package_info().version.to_string(), refresh, update::Channel::parse(&load_config(app).channel))
}

/// Patch notes for the launcher and the update for this build, if there is one. Never fails: an empty list at worst.
#[tauri::command]
async fn get_releases(app: AppHandle, refresh: Option<bool>) -> update::Releases {
    tauri::async_runtime::spawn_blocking(move || update::view_here(&releases_of(&app, refresh.unwrap_or(false)), update_version(&app).as_deref())).await.unwrap_or_default()
}

/// Play, Set up and the DLSS download write or run what an update is replacing.
fn refuse_during_update(app: &AppHandle) -> Result<(), String> {
    if app.state::<Arc<update::UpdateState>>().busy() {
        return Err("An update is being installed. Wait for it to finish.".into());
    }
    Ok(())
}

/// The launcher file itself: the AppImage, not the program inside its mount. macOS: the .app bundle, not the program inside it.
fn launcher_path() -> Option<PathBuf> {
    #[cfg(target_os = "macos")]
    if let Some(app) = host::bundle_of(&exe_dir()) {
        return Some(app);
    }
    std::env::var_os("APPIMAGE").map(PathBuf::from).or_else(|| std::env::current_exe().ok())
}

fn stop_relay(app: &AppHandle) {
    // take it out first: stopping joins threads, the lock is not held meanwhile
    let srv = app.state::<RelayState>().0.lock().unwrap().take();
    if let Some(mut srv) = srv {
        srv.stop();
    }
    // the router port opened for it (portmap.rs) closes with it, on the lease's own thread
    let lease = app.state::<RelayLease>().0.lock().unwrap().take();
    if let Some(lease) = lease {
        lease.finish(std::time::Duration::ZERO);
    }
}

/// A build the player picked (update.rs target_for_tag) instead of the newest newer one. `accepted`: the player confirmed the warning for
/// an older build (the dialogs ask; this side refuses without it).
struct Wanted {
    tag: String,
    channel: update::Channel,
    accepted: bool,
}

fn releases_for(app: &AppHandle, refresh: bool, channel: update::Channel) -> Vec<update::GhRelease> {
    let cache = config_path(app).map(|p| p.with_file_name("releases.json")).unwrap_or_default();
    update::load_releases(&app.state::<update::ReleaseStore>(), &cache, &app.package_info().version.to_string(), refresh, channel)
}

fn run_update(app: &AppHandle, st: &update::UpdateState, wanted: Option<Wanted>) -> Result<(), String> {
    let _claim = st.claim().ok_or("An update is already running.")?;
    // asked again right before files move: the download takes minutes
    let idle = || -> Result<(), String> {
        if app.state::<Arc<SetupState>>().progress.lock().unwrap().state == "running" {
            return Err("Setup is running. Pause it first, then update.".into());
        }
        if app.state::<play::PlayState>().0.lock().unwrap().state == "running" || saveimport::game_running() {
            return Err("The game is running. Close it first, then update.".into());
        }
        Ok(())
    };
    idle()?;
    let version = update_version(app).ok_or("This build does not update itself.")?;
    // the newest newer build, or the one the player picked
    let (target, older) = match &wanted {
        None => (update::target_here(&releases_of(app, false), &version).ok_or("No update is available right now.")?, false),
        Some(w) => {
            let t = update::target_for_tag_here(&releases_for(app, false, w.channel), &w.tag).ok_or("That build cannot be installed on this system.")?;
            (t, update::check_pick(&version, &w.tag, w.accepted)?)
        }
    };
    let paths = update::Paths {
        install: install_dir(),
        launcher: launcher_path().ok_or("Could not find the launcher's own file.")?,
        game_dir: game_dir_of(&load_config(app)),
    };
    let mut report = |phase: &str, done: u64, total: u64| {
        let _ = app.emit("update-progress", UpdateProgress { phase: phase.to_string(), done, total });
    };
    // an older build: the player's data is copied first (rollback.rs), after the download and right before the files move; if the copy
    // cannot be made nothing is replaced
    let config_dir = config_path(app).ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default();
    let ready = || -> Result<(), String> {
        idle()?;
        if older {
            rollback::backup(&paths.game_dir, &config_dir, &target.tag)?;
        }
        Ok(())
    };
    let session = update::install(&target, &paths, st, &mut report, &ready)?;
    report("restart", 0, 1);
    stop_relay(app);
    if let Err(e) = update::relaunch(&paths.launcher, &paths.install) {
        return Err(match session.rollback() {
            Ok(()) => format!("The new launcher did not start ({}). The update was undone.", e),
            Err(left) => format!("The new launcher did not start ({}). {}", e, left),
        });
    }
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.hide();
    }
    session.finish();
    app.exit(0);
    Ok(())
}

#[derive(Serialize, Clone)]
struct UpdateProgress {
    /// "download", "verify", "extract", "apply" or "restart"
    phase: String,
    done: u64,
    total: u64,
}

/// One-click update (update.rs). Emits "update-progress"; on success the launcher restarts and the page hears nothing more.
/// The Err of a cancel is update::CANCELLED.
#[tauri::command]
async fn install_update(app: AppHandle, state: State<'_, Arc<update::UpdateState>>) -> Result<(), String> {
    let st = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || run_update(&app, &st, None)).await.map_err(|e| e.to_string())?
}

/// The builds a channel offers, newest first, with their notes and what going to each one means (update.rs builds). Works for Testing
/// before the channel is saved, so the join question can list them. Empty for a build that takes no updates.
#[tauri::command]
async fn list_builds(app: AppHandle, channel: String, refresh: Option<bool>) -> Vec<update::Build> {
    let Some(version) = update_version(&app) else { return Vec::new() };
    tauri::async_runtime::spawn_blocking(move || update::builds_here(&releases_for(&app, refresh.unwrap_or(false), update::Channel::parse(&channel)), &version)).await.unwrap_or_default()
}

/// Installs the build the player picked (an older one only with `accepted`); the same steps and restart as the update.
#[tauri::command]
async fn install_version(app: AppHandle, state: State<'_, Arc<update::UpdateState>>, tag: String, channel: String, accepted: bool) -> Result<(), String> {
    let st = state.inner().clone();
    let wanted = Wanted { tag, channel: update::Channel::parse(&channel), accepted };
    tauri::async_runtime::spawn_blocking(move || run_update(&app, &st, Some(wanted))).await.map_err(|e| e.to_string())?
}

/// Stops a running update while it is still downloading or unpacking; the install stays as it was.
#[tauri::command]
fn cancel_update(state: State<Arc<update::UpdateState>>) {
    state.cancel();
}

/// Opens a release page in the default browser.
#[tauri::command]
fn open_release_url(url: String) -> Result<(), String> {
    if !openable_link(&url) {
        return Err("That link cannot be opened from here.".into());
    }
    play::open_url(&url)
}

/// The links the launcher opens in the browser: GitHub only (releases, issues, the project it is based on). No spaces, quotes or `&`.
fn openable_link(url: &str) -> bool {
    url.starts_with("https://github.com/") && !url.chars().any(|c| c.is_whitespace() || c == '"' || c == '&')
}

#[cfg(test)]
mod link_tests {
    use super::openable_link;

    #[test]
    fn only_github_opens() {
        assert!(openable_link("https://github.com/Chaos192/PokoMac/releases/tag/v1.0.0"));
        assert!(openable_link("https://github.com/Dittobyte/Recompia"));
        for bad in ["http://github.com/x", "https://github.com.evil.example/x", "https://discord.gg/gN5g4MtWHr", "https://discord.com/invite/x", "https://github.com/a b", "https://github.com/a&b", "file:///etc/passwd", "javascript:alert(1)", ""] {
            assert!(!openable_link(bad), "{bad:?}");
        }
    }
}

fn overview_of(cfg: &Config) -> Overview {
    let dir = game_dir_of(cfg);
    Overview {
        game_dir: dump::fwd(&dir),
        default_game_dir: dump::fwd(&default_game_dir()),
        exe_exists: dir.join(host::GAME_EXE).is_file(),
        builds_exe: gamebuild::is_package_b(&tools_dir()),
    }
}

#[tauri::command]
fn get_overview(app: AppHandle) -> Overview {
    overview_of(&load_config(&app))
}

#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
async fn pick_nup(app: AppHandle, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title("Choose your Pokopia .nup package").add_filter("NUP package", &["nup"]);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

/// Asks for the DLC .nsp ("Add DLC"); None when the dialog is closed.
#[tauri::command]
async fn pick_nsp(app: AppHandle) -> Option<String> {
    let dlg = app.dialog().file().set_title("Choose your Pokopia DLC .nsp").add_filter("NSP package", &["nsp"]);
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[derive(Serialize, Clone)]
struct NupProgress {
    done: u64,
    total: u64,
    file: String,
}

/// Unpacks a .nup into `<game folder>/nup_dump` (base/ and updates/<version>/, the same layout as a dump folder) and
/// returns that folder. Emits "nup-progress". A finished unpack of the same package is reused.
#[tauri::command]
async fn import_nup(app: AppHandle, state: State<'_, Arc<SetupState>>, path: String) -> Result<String, String> {
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Pause it first.".into());
    }
    let game_dir = game_dir_of(&load_config(&app));
    let out = game_dir.join("nup_dump");
    // a blocked folder fails here, in words, instead of halfway through with an OS error (writecheck.rs)
    writecheck::check(&game_dir).map_err(|e| e.message())?;
    let src = PathBuf::from(path.trim());
    let emitter = app.clone();
    let st = state.inner().clone();
    st.nup_cancel.store(false, Ordering::SeqCst);
    let res = tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        nup::extract(
            &src,
            &out,
            &mut |done, total, file| {
                // the UI only needs ~100 updates over the whole unpack
                if total == 0 || done == total || done.saturating_sub(last) >= (total / 100).max(1) {
                    last = done;
                    let _ = emitter.emit("nup-progress", NupProgress { done, total, file: file.to_string() });
                }
            },
            &|| st.nup_cancel.load(Ordering::SeqCst),
        )
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(dump::fwd(&res))
}

/// Unpacks the add-on (DLC) in a decrypted .nsp into the folder `[fs] aoc_dir` names (`<game folder>/aoc` when pkp.ini names none,
/// which is then written there) and returns the add-on ids. The game lists them at its next start.
#[tauri::command]
async fn import_dlc(app: AppHandle, state: State<'_, Arc<SetupState>>, path: String) -> Result<Vec<String>, String> {
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Pause it first.".into());
    }
    let game_dir = game_dir_of(&load_config(&app));
    writecheck::check(&game_dir).map_err(|e| e.message())?;
    let src = PathBuf::from(path.trim());
    tauri::async_runtime::spawn_blocking(move || nsp::extract(&src, &settings::aoc_target(&game_dir)?))
        .await
        .map_err(|e| e.to_string())?
}

/// Stops a running .nup unpack (the call to `import_nup` then returns an error).
#[tauri::command]
fn cancel_nup(state: State<Arc<SetupState>>) {
    state.nup_cancel.store(true, Ordering::SeqCst);
}

/// [fs] romfs_base for pkp.ini: the base romfs (relative to the game folder when inside it), or empty for none.
fn base_value(game_dir: &Path, base: &str) -> String {
    if base.is_empty() { String::new() } else { settings::portable(game_dir, base) }
}

#[tauri::command]
fn validate_dump(path: String) -> dump::DumpReport {
    dump::scan(&path)
}

/// Validates, stores the dump folder, and points `[fs] romfs` in pkp.ini at the chosen romfs.
#[tauri::command]
fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let report = dump::scan(&path);
    let chosen = report.chosen.clone().ok_or_else(|| report.errors.join(" "))?;
    let mut cfg = load_config(&app);
    cfg.dump_dir = report.path.clone();
    save_config(app.clone(), cfg.clone())?;
    settings::apply(
        &game_dir_of(&cfg),
        &[
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "exefs".into(), value: settings::portable(&game_dir_of(&cfg), &chosen.exefs) },
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "romfs".into(), value: settings::portable(&game_dir_of(&cfg), &chosen.romfs) },
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "romfs_base".into(), value: base_value(&game_dir_of(&cfg), &chosen.romfs_base) },
        ],
    )?;
    Ok(report)
}

#[tauri::command]
fn read_settings(app: AppHandle) -> settings::SettingsDump {
    settings::read(&game_dir_of(&load_config(&app)))
}

#[tauri::command]
fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    settings::apply(&game_dir_of(&load_config(&app)), &edits)
}

/// Whether Play can run, and if not, what the main button does about it. `kind`: no_files, bad_files, no_exe, setup, resume, rebuild,
/// retranslate, rebase, ready. `detail`: the sentence for the player (empty when ready).
#[derive(Serialize, Clone, Debug, PartialEq)]
struct Readiness {
    kind: &'static str,
    detail: String,
}

impl Readiness {
    fn of(kind: &'static str, detail: impl Into<String>) -> Readiness {
        Readiness { kind, detail: detail.into() }
    }
}

/// The game folder's state, once usable game files are chosen.
struct ReadyFacts {
    game_dir: String,
    exe_exists: bool,
    /// pkp.exe is there but pkp_rt.dll is neither beside it nor in the launcher's folder to copy from: it would stop at once (exit 53)
    runtime_missing: bool,
    /// the release builds pkp.exe itself
    builds_exe: bool,
    /// Current when the release does not build it
    built: gamebuild::Built,
    needs_retranslation: bool,
    shaders_complete: bool,
    /// an earlier setup started
    started: bool,
    /// the base romfs now layered under the update is not the one the shaders were made from (#128)
    base_changed: bool,
    /// the code mods are not the ones pkp was built with (mods.rs, docs/MODS.md)
    mods_changed: bool,
}

fn readiness_of(f: &ReadyFacts) -> Readiness {
    if !f.builds_exe && !f.exe_exists {
        return Readiness::of("no_exe", format!("{0} was not found in {1}. Put the launcher next to {0}.", host::GAME_EXE, f.game_dir));
    }
    if f.exe_exists && f.runtime_missing {
        #[cfg(target_os = "macos")]
        return Readiness::of("no_runtime", format!("libpkp_rt.dylib was not found next to pkp in {} nor in the launcher app (Contents/Resources/runtime). Download the release again and replace the app.", f.game_dir));
        #[cfg(not(target_os = "macos"))]
        return Readiness::of("no_runtime", format!("pkp_rt.dll was not found next to pkp.exe in {} nor next to the launcher. Extract the release again so the launcher's folder has it, or set the game folder in Settings.", f.game_dir));
    }
    let unfinished = || {
        if f.started {
            Readiness::of("resume", "Setup has not finished. Press Continue setup: it picks up where it stopped.")
        } else {
            Readiness::of("setup", "Press Set up to build the game and prepare its shaders.")
        }
    };
    match f.built {
        gamebuild::Built::Current => {}
        gamebuild::Built::Stale if f.mods_changed => return Readiness::of("mods_build", mods::CODE_CHANGED),
        gamebuild::Built::Stale => return Readiness::of("rebuild", gamebuild::REBUILD_MESSAGE),
        gamebuild::Built::No => return unfinished(),
    }
    if f.needs_retranslation {
        return Readiness::of("retranslate", "This version fixes how some shaders are translated. Press Finish update: it translates only those shaders again.");
    }
    if !f.shaders_complete {
        return unfinished();
    }
    if f.base_changed {
        return Readiness::of("rebase", "Your update holds only the files it changes, so the launcher now adds the missing ones from the base game. Press Finish update so their shaders are prepared too.");
    }
    Readiness::of("ready", "")
}

/// What Play writes into pkp.ini.
struct GamePaths {
    exefs: String,
    romfs: String,
    romfs_base: String,
    aoc_dir: String,
}

#[derive(Serialize, Clone)]
struct DumpChoice {
    path: String,
    /// "base", "update", "folder" or "nup"
    kind: String,
    /// the update's version folder; empty for the rest
    version: String,
}

struct Probe {
    readiness: Readiness,
    paths: Option<GamePaths>,
    dump: Option<DumpChoice>,
}

/// A setup has started in this game folder (Set up continues it).
fn setup_started(dir: &Path) -> bool {
    !shaders::read_marker(dir).romfs.is_empty() || dir.join("build").join("state.json").is_file()
}

/// The one answer to "can Play run": the Play command and the page's main button both come here, so they cannot disagree.
fn readiness(cfg: &Config) -> Probe {
    let refuse = |r: Readiness| Probe { readiness: r, paths: None, dump: None };
    let picked = cfg.dump_dir.trim();
    if picked.is_empty() {
        return refuse(Readiness::of("no_files", "Choose your game files first."));
    }
    // a .nup package is played as it is: pkp reads exefs and romfs straight from it
    let (paths, dump) = if picked.ends_with(".nup") && Path::new(picked).is_file() {
        let dump = DumpChoice { path: dump::fwd(Path::new(picked)), kind: "nup".into(), version: String::new() };
        (GamePaths { exefs: picked.to_string(), romfs: picked.to_string(), romfs_base: String::new(), aoc_dir: String::new() }, dump)
    } else {
        let report = dump::scan(&cfg.dump_dir);
        let Some(chosen) = report.chosen else {
            return refuse(Readiness::of("bad_files", format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" "))));
        };
        let dump = DumpChoice { path: chosen.path, kind: chosen.kind, version: chosen.version };
        (GamePaths { exefs: chosen.exefs, romfs: chosen.romfs, romfs_base: chosen.romfs_base, aoc_dir: report.aoc_dir }, dump)
    };
    let dir = game_dir_of(cfg);
    let tools_dir = tools_dir();
    let builds_exe = gamebuild::is_package_b(&tools_dir);
    // the base game's files now fill in an update that lacks them (#128): its shaders were never extracted, so Set up runs once more
    let base_changed = !paths.romfs_base.is_empty() && {
        let job = shaders::Job { game_dir: dir.clone(), romfs: PathBuf::from(&paths.romfs), romfs_base: Some(PathBuf::from(&paths.romfs_base)), main: PathBuf::new(), jobs: 1, idle: false, log: None, limit: 0, compact: false };
        shaders::read_marker(&dir).romfs != job.romfs_key()
    };
    let readiness = readiness_of(&ReadyFacts {
        game_dir: dump::fwd(&dir),
        exe_exists: dir.join(host::GAME_EXE).is_file(),
        runtime_missing: !dir.join(host::RUNTIME_LIB).is_file() && !host::runtime_source(&tools_dir).is_some_and(|s| s.join(host::RUNTIME_LIB).is_file()),
        builds_exe,
        built: if builds_exe { gamebuild::built_state(&dir, &tools_dir) } else { gamebuild::Built::Current },
        needs_retranslation: shaders::needs_retranslation(&dir),
        shaders_complete: shaders::is_complete(&dir),
        started: setup_started(&dir),
        base_changed,
        mods_changed: builds_exe && mods::code_changed(&dir),
    });
    Probe { readiness, paths: Some(paths), dump: Some(dump) }
}

#[derive(Serialize)]
struct LauncherState {
    readiness: Readiness,
    #[serde(flatten)]
    overview: Overview,
    dump: Option<DumpChoice>,
    version: String,
    /// this is the first look after a self-update restart
    updated: bool,
}

/// What the page's main button is built from. The same checks as Play, off the UI thread: they read the disk and can start Python.
#[tauri::command]
async fn launcher_state(app: AppHandle) -> Result<LauncherState, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let cfg = load_config(&app);
        let probe = readiness(&cfg);
        LauncherState { readiness: probe.readiness, overview: overview_of(&cfg), dump: probe.dump, version: app.package_info().version.to_string(), updated: UPDATED.swap(false, Ordering::SeqCst) }
    })
    .await
    .map_err(|e| e.to_string())
}

#[tauri::command]
fn play(app: AppHandle) -> Result<(), String> {
    refuse_during_update(&app)?;
    refuse_during_storage(&app)?;
    let cfg = load_config(&app);
    let probe = readiness(&cfg);
    let (Some(paths), "ready") = (probe.paths, probe.readiness.kind) else {
        return Err(probe.readiness.detail);
    };
    let dir = game_dir_of(&cfg);
    settings::ensure_play_paths(&dir, &paths.exefs, &paths.romfs, &paths.romfs_base, &paths.aoc_dir)?;
    profiles::apply_active(&dir)?;
    play::start(&app, &dir, cfg.minimize_on_play)
}

/// Shader setup: the running job's progress and its cancel flag.
#[derive(Default)]
struct SetupState {
    progress: Mutex<shaders::Progress>,
    cancel: AtomicBool,
    /// Cancel flag of a running .nup unpack.
    nup_cancel: AtomicBool,
}

#[derive(Serialize)]
struct SetupStatus {
    progress: shaders::Progress,
    /// setup.json says finished and the cache is there
    complete: bool,
    /// an earlier setup started (Set up continues it)
    started: bool,
    /// pkp.exe is from an older release whose game code this one changes: Set up rebuilds it
    rebuild: bool,
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<SetupState>>) -> SetupStatus {
    let dir = game_dir_of(&load_config(&app));
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    let tools_dir = tools_dir();
    let state = if gamebuild::is_package_b(&tools_dir) { gamebuild::built_state(&dir, &tools_dir) } else { gamebuild::Built::Current };
    let started = setup_started(&dir);
    SetupStatus {
        progress,
        complete: state == gamebuild::Built::Current && shaders::is_complete(&dir),
        started,
        rebuild: state == gamebuild::Built::Stale,
    }
}

/// First-run setup: (a) the pkp.ini paths, (b) in a model (b) release, pkp.exe built from the player's dump
/// (gamebuild.rs), (c) the shader cache in shader_cache/spirv, on a worker thread with "setup-status" events.
#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<SetupState>>, gentle: Option<bool>) -> Result<(), String> {
    refuse_during_update(&app)?;
    refuse_during_storage(&app)?;
    let cfg = load_config(&app);
    let gentle = gentle.unwrap_or(cfg.gentle_build);
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    let dir = game_dir_of(&cfg);
    let tools_dir = tools_dir();
    let package_b = gamebuild::is_package_b(&tools_dir);
    if !package_b && !dir.join(host::GAME_EXE).is_file() {
        return Err(format!("{0} was not found in {1}. Put the launcher next to {0} or set the game folder in Settings.", host::GAME_EXE, dump::fwd(&dir)));
    }
    // the build, the shader tools and pkp.ini all write in the game folder: a blocked one fails here, in words (writecheck.rs)
    writecheck::check(&dir).map_err(|e| e.message())?;
    {
        let mut p = state.progress.lock().unwrap();
        if p.state == "running" {
            return Err("Setup is already running.".into());
        }
        *p = shaders::Progress { state: "running".into(), phase: "check".into(), message: "Checking the setup tools...".into(), ..Default::default() };
    }
    settings::ensure_play_paths(&dir, &chosen.exefs, &chosen.romfs, &chosen.romfs_base, &report.aoc_dir)?;
    let st = state.inner().clone();
    st.cancel.store(false, Ordering::SeqCst);
    let cpus = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(3);
    let (jobs, build_jobs) = preflight::jobs_for(cpus, gentle);
    let slog = Arc::new(setuplog::SetupLog::open(&dir));
    let log: shaders::LogFn = {
        let l = slog.clone();
        Arc::new(move |t: &str| l.line(t))
    };
    log(&format!(
        "=== Set up started: launcher {}, {} threads, {} jobs{}",
        env!("CARGO_PKG_VERSION"),
        cpus,
        jobs,
        if gentle { ", gentle (idle priority)" } else { "" }
    ));
    log(&format!("game files: {}", dump::fwd(Path::new(&chosen.exefs))));
    let build_job = gamebuild::Job { game_dir: dir.clone(), exefs: PathBuf::from(&chosen.exefs), jobs: build_jobs, idle: gentle, log: Some(log.clone()) };
    let romfs_base = (!chosen.romfs_base.is_empty()).then(|| PathBuf::from(&chosen.romfs_base));
    let job = shaders::Job { game_dir: dir, romfs: PathBuf::from(&chosen.romfs), romfs_base, main: PathBuf::from(&chosen.exefs).join("main"), jobs, idle: gentle, log: Some(log.clone()), limit: 0, compact: !cfg.loose_shaders };
    std::thread::spawn(move || {
        let mut last = (String::new(), 0u64, String::new());
        let mut emit = |p: shaders::Progress| {
            if setuplog::progress_worth_logging(&mut last, &p.phase, p.done, p.total, &p.message) && p.state == "running" {
                log(&format!("[{}] {}", p.phase, p.message));
            }
            *st.progress.lock().unwrap() = p.clone();
            let _ = app.emit("setup-status", p);
        };
        let result = (|| {
            // Linux and macOS: the native Python and Zig that the build and the shader extractor run with, fetched once
            let tools_ready = host::prepare_tools(&tools_dir, &st.cancel, &mut |done, total, message| {
                emit(shaders::Progress { state: "running".into(), phase: "tools".into(), done, total, message })
            })?;
            if !tools_ready {
                return Ok(false);
            }
            // Linux: the translator and the hidden run need Proton; say so now, not after the build (macOS runs both natively)
            #[cfg(not(any(windows, target_os = "macos")))]
            log(&format!("Windows programs run with: {}", host::runner()?.label));
            // no build yet, or one from an older release (stale tools stamp): build pkp.exe (again)
            if package_b && !gamebuild::is_built(&build_job.game_dir, &tools_dir) {
                let tools = gamebuild::find_tools(&tools_dir)?;
                let built = gamebuild::run(&build_job, &tools, &st.cancel, |t| {
                    emit(shaders::Progress { state: "running".into(), phase: t.phase, done: t.done, total: t.total, message: t.message })
                })?;
                if !built {
                    return Ok(false);
                }
            }
            let tools = shaders::find_tools(&tools_dir)?;
            // a finished cache of the same romfs has nothing left to translate (re-extracting would only find that out)
            let same = shaders::read_marker(&job.game_dir).romfs == job.romfs_key();
            let done = same && shaders::is_complete(&job.game_dir);
            if !done && !shaders::run(&job, &tools, &st.cancel, |p| emit(p))? {
                return Ok(false);
            }
            // the shaders the game builds while it runs: one hidden run collects those of the opening; later ones are
            // collected by every normal run, and every Play checks them all (play.rs)
            emit(shaders::Progress {
                state: "running".into(),
                phase: "warmup".into(),
                message: format!("Running the game once in the background to collect the shaders it builds while it runs (about {} s)...", shaders::WARMUP_SECONDS),
                ..Default::default()
            });
            if shaders::prepare(&job.game_dir, &tools_dir, &st.cancel)?.is_none() {
                return Ok(false);
            }
            // macOS: the pipelines of the opening, the character creator and the first area compiled for Metal now, not while
            // the player waits for them in the game (shaders.rs prepare_pipelines)
            if cfg!(target_os = "macos") {
                emit(shaders::Progress {
                    state: "running".into(),
                    phase: "pipelines".into(),
                    message: "Compiling the game's graphics for Metal, so nothing is missing the first time you play...".into(),
                    ..Default::default()
                });
                let built = shaders::prepare_pipelines(&job.game_dir, &tools_dir, &st.cancel, job.log.as_ref(), |done, total| {
                    emit(shaders::Progress {
                        state: "running".into(),
                        phase: "pipelines".into(),
                        done,
                        total,
                        message: format!("Compiling the game's graphics for Metal: {} of {}", done, total),
                    })
                })?;
                if built.is_none() {
                    return Ok(false);
                }
            }
            Ok(true)
        })();
        let end = match result {
            Ok(true) => shaders::Progress { state: "done".into(), phase: "finish".into(), message: "Setup finished.".into(), ..Default::default() },
            Ok(false) => shaders::Progress { state: "cancelled".into(), message: "Setup paused. Set up again continues where it stopped.".into(), ..Default::default() },
            Err(e) => shaders::Progress { state: "error".into(), message: e, ..Default::default() },
        };
        slog.line(&format!("=== Set up ended: {}{}", end.state, if end.message.is_empty() { String::new() } else { format!(": {}", end.message) }));
        emit(end);
    });
    Ok(())
}

/// The checks shown before Set up starts (preflight.rs). Reads the disk and loads Vulkan, so it runs off the UI thread.
#[tauri::command]
async fn preflight(app: AppHandle) -> Result<preflight::Report, String> {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg);
    let compact = !cfg.loose_shaders;
    let tools_dir = tools_dir();
    tauri::async_runtime::spawn_blocking(move || {
        let build_pending = gamebuild::is_package_b(&tools_dir) && !gamebuild::is_built(&dir, &tools_dir);
        let facts = preflight::probe(&dir, &preflight::remaining_of(&dir, build_pending, compact));
        let report = preflight::evaluate(&facts);
        // into the setup log too: a failed setup report then shows what the PC looked like
        let log = setuplog::SetupLog::open(&dir);
        for c in &report.checks {
            log.line(&format!("preflight {:?}: {}: {}", c.level, c.title, c.detail));
        }
        report
    })
    .await
    .map_err(|e| e.to_string())
}

/// Opens setup.log (next to pkp.exe) in the default editor.
#[tauri::command]
fn open_setup_log(app: AppHandle) -> Result<(), String> {
    let path = setuplog::path_of(&game_dir_of(&load_config(&app)));
    if !path.is_file() {
        return Err("There is no setup log yet: it is written when Set up starts.".into());
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn cancel_setup(state: State<Arc<SetupState>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

/// "Make shader files smaller" and its undo (shader_archive.rs): one at a time, never while the game, setup or an update runs.
#[derive(Default)]
struct StorageState {
    running: AtomicBool,
    cancel: AtomicBool,
}

fn refuse_during_storage(app: &AppHandle) -> Result<(), String> {
    if app.state::<Arc<StorageState>>().running.load(Ordering::SeqCst) {
        return Err("The shader files are being repacked. Wait for it to finish, or stop it in Settings.".into());
    }
    Ok(())
}

#[derive(Serialize, Clone)]
struct StorageProgress {
    /// "compact" or "expand"
    kind: String,
    done: u64,
    total: u64,
}

#[derive(Serialize)]
struct StorageStatus {
    #[serde(flatten)]
    storage: shader_archive::Storage,
    /// setup finished, so a move now is the whole cache (the launcher offers it); during or before setup it is not offered
    complete: bool,
    running: bool,
}

/// How the translated shaders are stored. Lists the cache folder, so it runs off the UI thread.
#[tauri::command]
async fn shader_storage(app: AppHandle, state: State<'_, Arc<StorageState>>) -> Result<StorageStatus, String> {
    let dir = game_dir_of(&load_config(&app));
    let running = state.running.load(Ordering::SeqCst);
    tauri::async_runtime::spawn_blocking(move || StorageStatus { storage: shader_archive::storage(&dir), complete: shaders::is_complete(&dir), running })
        .await
        .map_err(|e| e.to_string())
}

/// Starts a storage job after the checks every one of them needs. The guard clears `running` however the job ends.
fn begin_storage(app: &AppHandle) -> Result<(PathBuf, Arc<StorageState>), String> {
    refuse_during_update(app)?;
    if app.state::<Arc<SetupState>>().progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Wait for it to finish, or pause it first.".into());
    }
    if app.state::<play::PlayState>().0.lock().unwrap().state == "running" || saveimport::game_running() {
        return Err("The game is running. Close it first: it reads the shader files while it runs.".into());
    }
    let st = app.state::<Arc<StorageState>>().inner().clone();
    if st.running.swap(true, Ordering::SeqCst) {
        return Err("The shader files are already being repacked.".into());
    }
    st.cancel.store(false, Ordering::SeqCst);
    Ok((game_dir_of(&load_config(app)), st))
}

struct StorageRun(Arc<StorageState>);
impl Drop for StorageRun {
    fn drop(&mut self) {
        self.0.running.store(false, Ordering::SeqCst);
    }
}

#[derive(Serialize)]
struct CompactResult {
    #[serde(flatten)]
    moved: shader_archive::Moved,
    finished: bool,
}

/// Moves every loose shader file into the shader archive. Nothing is translated: the files are compressed as they are. Emits
/// "shader-storage-progress"; resolves when done or stopped (cancel_shader_storage). What was moved stays moved.
#[tauri::command]
async fn compact_shaders(app: AppHandle) -> Result<CompactResult, String> {
    let (dir, st) = begin_storage(&app)?;
    let run = StorageRun(st.clone());
    let slog = setuplog::SetupLog::open(&dir);
    let (moved, finished) = tauri::async_runtime::spawn_blocking(move || {
        let _run = run;
        writecheck::check(&dir).map_err(|e| e.message())?;
        let before = shader_archive::storage(&dir);
        slog.line(&format!("=== Make shader files smaller: {} loose files ({:.1} GB)", before.loose_files, before.loose_bytes as f64 / preflight::GB as f64));
        let r = shader_archive::compact(&dir, None, &st.cancel, |done, total| {
            let _ = app.emit("shader-storage-progress", StorageProgress { kind: "compact".into(), done, total });
        });
        // a later Set up again keeps the cache packed (shaders.rs Marker::storage)
        if r.is_ok() {
            shaders::set_storage(&dir, "compact")?;
        }
        match &r {
            Ok((m, fin)) => slog.line(&format!(
                "=== {}: {} shaders, {:.2} GB of SPIR-V in {:.2} GB ({:.2}x), {:.0} s",
                if *fin { "Done" } else { "Stopped" },
                m.shaders,
                m.raw_bytes as f64 / preflight::GB as f64,
                m.disk_bytes as f64 / preflight::GB as f64,
                if m.disk_bytes > 0 { m.raw_bytes as f64 / m.disk_bytes as f64 } else { 0.0 },
                m.seconds
            )),
            Err(e) => slog.line(&format!("=== Make shader files smaller failed: {}", e)),
        }
        r
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(CompactResult { moved, finished })
}

/// The undo: writes the archive back out as loose files. Needs the free space of the loose files (about 26 GB).
#[tauri::command]
async fn expand_shaders(app: AppHandle) -> Result<bool, String> {
    let (dir, st) = begin_storage(&app)?;
    let run = StorageRun(st.clone());
    tauri::async_runtime::spawn_blocking(move || {
        let _run = run;
        let s = shader_archive::storage(&dir);
        // segments are deleted as they are written out: the peak is the loose size less what the archive already took
        let need = s.archive_raw_bytes.saturating_sub(s.archive_bytes) + preflight::GB;
        if let Some(free) = preflight::free_bytes(&dir) {
            if free < need {
                return Err(format!(
                    "Loose shader files need about {:.0} GB more free space on {} ({:.0} GB free).",
                    need as f64 / preflight::GB as f64,
                    preflight::drive_of(&dir),
                    free as f64 / preflight::GB as f64
                ));
            }
        }
        let finished = shader_archive::expand(&dir, &st.cancel, |done, total| {
            let _ = app.emit("shader-storage-progress", StorageProgress { kind: "expand".into(), done, total });
        })?;
        if finished {
            shaders::set_storage(&dir, "loose")?;
        }
        Ok(finished)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn cancel_shader_storage(state: State<Arc<StorageState>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

#[tauri::command]
fn dlss_status(app: AppHandle) -> dlss::Status {
    dlss::status(&game_dir_of(&load_config(&app)), &tools_dir())
}

/// Downloads NVIDIA's nvngx_dlss.dll (see dlss.rs) on a worker thread, with "dlss-progress" events.
#[tauri::command]
async fn fetch_dlss(app: AppHandle) -> Result<(), String> {
    refuse_during_update(&app)?;
    let dir = game_dir_of(&load_config(&app));
    let tools_dir = tools_dir();
    tauri::async_runtime::spawn_blocking(move || {
        dlss::fetch(&dir, &tools_dir, |p| {
            let _ = app.emit("dlss-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Where the runtime keeps the save and which account folder the game uses, from pkp.ini.
fn save_paths(app: &AppHandle) -> (PathBuf, String) {
    let dir = game_dir_of(&load_config(app));
    let pkp = settings::read(&dir).pkp;
    let get = |k: &str| pkp.get(k).cloned().unwrap_or_default();
    // NXRT_FS_SAVE_DIR overrides the ini in the runtime, so it does here
    let env = std::env::var("NXRT_FS_SAVE_DIR").unwrap_or_default();
    let sd = saveimport::save_dir_of(&dir, &if env.trim().is_empty() { get("fs.save_dir") } else { env });
    // with profiles, a profile other than the first plays its own account folder, not the newest one
    let key = profiles::strict_save_key(&dir).unwrap_or_else(|| saveimport::target_key(&sd, &get("account.uid")));
    (sd, key)
}

/// Eden and Ryujinx saves of the game found on this PC, newest first.
#[tauri::command]
fn save_sources() -> Vec<saveimport::Source> {
    saveimport::find_sources_here()
}

/// Imports a save: `source` is a picked folder, or None for the newest one found in Eden / Ryujinx.
#[tauri::command]
fn import_save(app: AppHandle, state: State<play::PlayState>, source: Option<String>) -> Result<saveimport::Report, String> {
    if state.0.lock().unwrap().state == "running" || saveimport::game_running() {
        return Err("The game is running. Close it first, then import the save.".into());
    }
    let src = match source.filter(|s| !s.trim().is_empty()) {
        Some(p) => saveimport::resolve_picked(Path::new(p.trim()))
            .ok_or_else(|| format!("{} holds no Pokopia save (it needs the G00 and S00 folders).", p.trim()))?,
        None => {
            let found = saveimport::find_sources_here();
            let first = found.first().ok_or("No Pokopia save found in Eden or Ryujinx. Use \"Pick a folder\" instead.")?;
            PathBuf::from(&first.path)
        }
    };
    let (sd, key) = save_paths(&app);
    saveimport::import(&sd, &key, &src)
}

/// Repair and reset (repair.rs). Planning and reading the last backup change nothing; applying and undoing need the game, Setup and
/// updates to be idle. Returns the game folder and the launcher's own settings folder, the only two places a repair may touch.
fn repair_paths(app: &AppHandle) -> Result<(PathBuf, PathBuf), String> {
    let cfg = config_path(app)?.parent().map(Path::to_path_buf).ok_or("The launcher settings folder is not known.")?;
    Ok((game_dir_of(&load_config(app)), cfg))
}

fn repair_guard(app: &AppHandle, play: &State<play::PlayState>, setup: &State<SetupState>) -> Result<(PathBuf, PathBuf), String> {
    if play.0.lock().unwrap().state == "running" || saveimport::game_running() {
        return Err("The game is running. Close it first, then repair.".into());
    }
    refuse_during_update(app)?;
    if setup.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Wait for it to finish, then repair.".into());
    }
    repair_paths(app)
}

#[tauri::command]
fn repair_plan(app: AppHandle, actions: Vec<String>, groups: Option<Vec<String>>) -> Result<repair::Plan, String> {
    let (game, cfg) = repair_paths(&app)?;
    repair::plan_with(&game, &cfg, &actions, groups.as_deref())
}

#[tauri::command]
fn repair_apply(app: AppHandle, play: State<play::PlayState>, setup: State<SetupState>, actions: Vec<String>, groups: Option<Vec<String>>) -> Result<repair::Report, String> {
    let (game, cfg) = repair_guard(&app, &play, &setup)?;
    repair::apply_with(&game, &cfg, &actions, groups.as_deref())
}

#[tauri::command]
fn repair_undo(app: AppHandle, play: State<play::PlayState>, setup: State<SetupState>) -> Result<repair::Report, String> {
    let (game, cfg) = repair_guard(&app, &play, &setup)?;
    repair::undo(&game, &cfg)
}

#[tauri::command]
fn repair_last(app: AppHandle) -> Result<Option<repair::BackupInfo>, String> {
    let (game, _) = repair_paths(&app)?;
    Ok(repair::last_backup(&game))
}

/// Save backups (savebackup.rs): the status, the list and a copy on request work at any time; a restore needs the game closed. They run
/// on a worker thread because a copy can wait a moment for the runtime to finish writing the save.
#[tauri::command]
async fn backup_status(app: AppHandle) -> Result<savebackup::Status, String> {
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::status(&dir)).await.map_err(|e| e.to_string())
}

#[tauri::command]
async fn backup_list(app: AppHandle) -> Result<Vec<savebackup::Entry>, String> {
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::list(&dir)).await.map_err(|e| e.to_string())
}

#[tauri::command]
async fn backup_now(app: AppHandle) -> Result<savebackup::Outcome, String> {
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::snapshot(&dir, savebackup::KIND_MANUAL)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn backup_restore(app: AppHandle, name: String) -> Result<savebackup::Restored, String> {
    if app.state::<play::PlayState>().0.lock().unwrap().state == "running" || saveimport::game_running() {
        return Err("The game is running. Close it first, then restore a backup.".into());
    }
    refuse_during_update(&app)?;
    let dir = game_dir_of(&load_config(&app));
    tauri::async_runtime::spawn_blocking(move || savebackup::restore(&dir, &name)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
fn open_backup_dir(app: AppHandle) -> Result<(), String> {
    let dir = savebackup::backup_root(&game_dir_of(&load_config(&app)));
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    play::open_with_shell(&dir)
}

/// The folder the game's photos go to (pkp.ini `[album] dir`, relative to the game folder; `album` by default, runtime album.cpp).
#[tauri::command]
fn open_album_dir(app: AppHandle) -> Result<(), String> {
    let game = game_dir_of(&load_config(&app));
    let ini = fs::read(game.join(settings::PKP_INI)).ok().map(|b| crate::ini::Ini::parse(&String::from_utf8_lossy(&b)));
    let set = ini.as_ref().and_then(|i| i.get("album", "dir")).unwrap_or_default();
    let set = if set.trim().is_empty() { "album".to_string() } else { set.trim().to_string() };
    let p = PathBuf::from(&set);
    let dir = if p.is_absolute() { p } else { game.join(p) };
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    play::open_with_shell(&dir)
}

/// Add to Steam (steam.rs): what is found, and whether the game and the launcher are already in Steam.
#[tauri::command]
async fn steam_status(app: AppHandle) -> Result<steam::Status, String> {
    let testing = load_config(&app).channel == "testing";
    tauri::async_runtime::spawn_blocking(move || steam::status(testing)).await.map_err(|e| e.to_string())
}

/// `target`: "game" (starts the game at once) or "launcher" (opens this window).
#[tauri::command]
async fn steam_add(app: AppHandle, target: String) -> Result<String, String> {
    let testing = load_config(&app).channel == "testing";
    let target = steam::Target::parse(&target)?;
    tauri::async_runtime::spawn_blocking(move || steam::add(testing, target)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn steam_remove(app: AppHandle, target: String) -> Result<String, String> {
    let testing = load_config(&app).channel == "testing";
    let target = steam::Target::parse(&target)?;
    tauri::async_runtime::spawn_blocking(move || steam::remove(testing, target)).await.map_err(|e| e.to_string())?
}

/// The profiles (names and ids) and which one plays.
#[tauri::command]
fn profiles_list(app: AppHandle) -> profiles::Store {
    profiles::load(&game_dir_of(&load_config(&app)))
}

fn profiles_guard(app: &AppHandle, state: &State<play::PlayState>) -> Result<PathBuf, String> {
    if state.0.lock().unwrap().state == "running" || saveimport::game_running() {
        return Err("The game is running. Close it first, then change profiles.".into());
    }
    refuse_during_update(app)?;
    Ok(game_dir_of(&load_config(app)))
}

#[tauri::command]
fn profile_add(app: AppHandle, state: State<play::PlayState>, name: String) -> Result<profiles::Store, String> {
    profiles::add(&profiles_guard(&app, &state)?, &name)
}

#[tauri::command]
fn profile_rename(app: AppHandle, state: State<play::PlayState>, id: String, name: String) -> Result<profiles::Store, String> {
    let dir = profiles_guard(&app, &state)?;
    let s = profiles::rename(&dir, &id, &name)?;
    profiles::apply_active(&dir)?;
    Ok(s)
}

#[tauri::command]
fn profile_remove(app: AppHandle, state: State<play::PlayState>, id: String) -> Result<profiles::Store, String> {
    profiles::remove(&profiles_guard(&app, &state)?, &id)
}

#[tauri::command]
fn profile_select(app: AppHandle, state: State<play::PlayState>, id: String) -> Result<profiles::Store, String> {
    profiles::select(&profiles_guard(&app, &state)?, &id)
}

#[tauri::command]
fn open_save_dir(app: AppHandle) -> Result<(), String> {
    let (sd, _) = save_paths(&app);
    fs::create_dir_all(&sd).map_err(|e| e.to_string())?;
    play::open_with_shell(&sd)
}

#[tauri::command]
fn game_status(state: State<play::PlayState>) -> play::Status {
    state.0.lock().unwrap().clone()
}

#[tauri::command]
fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !play::REPORT_FILES.contains(&name.as_str()) {
        return Err("That file cannot be opened from here.".into());
    }
    let path = game_dir_of(&load_config(&app)).join(&name);
    if !path.is_file() {
        return Err(format!("{} does not exist.", name));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn open_game_dir(app: AppHandle) -> Result<(), String> {
    play::open_with_shell(&game_dir_of(&load_config(&app)))
}

/// Report a problem (report.rs): the logs and settings in a zip on the Desktop, shown in Finder, and a new GitHub issue with the
/// system facts filled in. Runs off the UI thread: it reads the logs and asks the system about the Mac.
#[tauri::command]
async fn report_problem(app: AppHandle) -> Result<report::Made, String> {
    let dir = game_dir_of(&load_config(&app));
    let cfg = config_path(&app)?;
    let version = app.package_info().version.to_string();
    let last = {
        let s = app.state::<play::PlayState>().0.lock().unwrap().clone();
        (s.state == "exited").then_some(s.message)
    };
    tauri::async_runtime::spawn_blocking(move || report::make(&dir, &cfg, &version, last.as_deref())).await.map_err(|e| e.to_string())?
}

/// iCloud Drive save sync (icloud.rs) for Settings: whether this Mac has iCloud Drive, and when the save last synced. Reads sync.json,
/// which iCloud may first have to download, so it runs off the UI thread.
#[tauri::command]
async fn icloud_status(app: AppHandle) -> Result<icloud::Status, String> {
    let dir = game_dir_of(&load_config(&app));
    let cfg = config_path(&app)?;
    tauri::async_runtime::spawn_blocking(move || icloud::status(&dir, &cfg)).await.map_err(|e| e.to_string())
}

/// Opens PokoMac's folder in iCloud Drive (the saves, and copies kept after a conflict) in Finder.
#[tauri::command]
fn open_icloud_dir() -> Result<(), String> {
    let dir = icloud::folder().ok_or("iCloud Drive is not available on this Mac.")?;
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dir.display(), e))?;
    play::open_with_shell(&dir)
}


/// Test runs only (PKP_LAUNCHER_EVAL_FILE set): the page writes a result next to the eval file, <file>.out.
#[tauri::command]
fn test_report(text: String) -> Result<(), String> {
    let path = std::env::var("PKP_LAUNCHER_EVAL_FILE").map_err(|_| "not a test run".to_string())?;
    std::fs::write(format!("{path}.out"), text).map_err(|e| e.to_string())
}

/// The relay server the launcher runs on a thread (Host button, phase 4). None: not running.
#[derive(Default)]
struct RelayState(Mutex<Option<relay::RelayServer>>);

/// The router port opened for the relay while it runs (portmap.rs). None: not asked for (forwarding off, or no relay).
#[derive(Default)]
struct RelayLease(Mutex<Option<portmap::Lease>>);

/// Opens the relay's port on the router, unless the player switched that off or it is open already. Never waits: the lease asks on its
/// own thread.
fn lease_relay_port(app: &AppHandle, port: u16) {
    if !config_path(app).map_or(true, |c| portmap::enabled(&c)) {
        return;
    }
    let leases = app.state::<RelayLease>();
    let mut slot = leases.0.lock().unwrap();
    match slot.as_ref().map(portmap::Lease::port) {
        Some(p) if p == port => {}
        _ => {
            if let Some(old) = slot.take() {
                old.finish(std::time::Duration::ZERO);
            }
            *slot = Some(portmap::Lease::start("relay", port));
        }
    }
}

/// Starts the relay (pokopia-native's coop-relay protocol) on 0.0.0.0:port (0 or none: 47610). A running one is stopped first.
#[tauri::command]
fn relay_start(app: AppHandle, state: State<RelayState>, port: Option<u16>) -> Result<relay::Status, String> {
    let mut slot = state.0.lock().unwrap();
    if let Some(mut old) = slot.take() {
        old.stop();
    }
    let port = port.filter(|p| *p != 0).unwrap_or(relay::DEFAULT_PORT);
    let srv = relay::RelayServer::start(("0.0.0.0", port), relay::Config::default(), 0).map_err(|e| format!("Cannot open port {}: {}", port, e))?;
    let status = srv.status();
    let bound = srv.local_addr().port();
    *slot = Some(srv);
    // visitors over the internet reach this relay through the router: its port is opened there while it runs (portmap.rs)
    lease_relay_port(&app, bound);
    Ok(status)
}

#[tauri::command]
fn relay_stop(app: AppHandle) {
    stop_relay(&app);
}

#[tauri::command]
fn relay_status(state: State<RelayState>) -> Option<relay::Status> {
    state.0.lock().unwrap().as_ref().map(|s| s.status())
}

/// LAN address to give visitors, other addresses, and the Windows Public-network warning (read only).
#[tauri::command]
fn lan_report() -> lan::LanReport {
    lan::report()
}

/// This PC's public IPv4 address for an internet host's visitors, or None when it cannot be found (offline, a service down).
#[tauri::command]
async fn public_ip() -> Option<String> {
    tauri::async_runtime::spawn_blocking(public_ip::lookup).await.ok().flatten()
}

/// Automatic port forwarding (portmap.rs): whether it is on, and how the router answered for the game and the relay.
#[tauri::command]
fn port_forward_status(app: AppHandle) -> portmap::Report {
    portmap::report(config_path(&app).ok().as_deref())
}

/// The Link Play page's switch. Off: every port the launcher opened is closed now. On: the relay's, if it runs, is asked for now; the
/// game's when the game next starts.
#[tauri::command]
fn port_forward_set(app: AppHandle, relay: State<RelayState>, enabled: bool) -> Result<portmap::Report, String> {
    let mut cfg = load_config(&app);
    cfg.port_forward_off = !enabled;
    save_config(app.clone(), cfg)?;
    if enabled {
        let running = relay.0.lock().unwrap().as_ref().map(|s| s.local_addr().port());
        if let Some(port) = running {
            lease_relay_port(&app, port);
        }
    } else {
        portmap::cancel_all();
        let lease = app.state::<RelayLease>().0.lock().unwrap().take();
        if let Some(lease) = lease {
            lease.finish(std::time::Duration::ZERO);
        }
    }
    Ok(portmap::report(config_path(&app).ok().as_deref()))
}

/// A new room code for a host, XXXX-XXXX-XXXX-XXXX (80 bits from the OS random source). The launcher writes it to pkp.ini
/// `[net] room_code` so the game hosts with it and the page can show it before the game starts.
#[tauri::command]
fn room_code_new() -> Result<String, String> {
    relay::room_code::new_code().map(|c| relay::room_code::format(&c)).map_err(|e| format!("Cannot make a room code: {}", e))
}

/// A visitor's typed code: the display form, or the error text to show.
#[tauri::command]
fn room_code_check(code: String) -> Result<String, String> {
    relay::room_code::normalize(&code)
        .map(|c| relay::room_code::format(&c))
        .ok_or_else(|| "A room code has 16 letters and digits: XXXX-XXXX-XXXX-XXXX.".to_string())
}

/// The room code the running game uses, from `<game folder>/room-code.txt` (written by the game when it starts local wireless
/// with `[net] room_code` set, removed when it runs without one). None: no file or no valid code in it.
#[tauri::command]
fn room_code_host(app: AppHandle) -> Option<String> {
    let text = fs::read_to_string(game_dir_of(&load_config(&app)).join("room-code.txt")).ok()?;
    relay::room_code::normalize(text.trim()).map(|c| relay::room_code::format(&c))
}

/// `--host [--listen <IPv4>[:<port>]] [--port N] [--no-udp] [limits] [--stats S] [--quiet]`: the relay alone, no window, log lines on stdout,
/// until Ctrl+C. Listens on 0.0.0.0:47610 by default. The limits are pokopia-native's coop-relay options (--rooms --peers --links --per-ip
/// --new-per-ip --nofind-per-min --room-hours --room-mib --link-kib --wait-s --idle-s). `--no-udp`: no relay UDP (game datagrams stay on the
/// TCP links, as with pokopia-native's relay).
fn run_headless(args: &[String]) -> i32 {
    #[cfg(windows)]
    win_console::attach_and_hook_ctrl_c();
    const USAGE: &str = "Usage: PokoMac --host [--listen <IPv4>[:<port>]] [--port N] [--no-udp] [--rooms N] [--peers N] [--links N] [--per-ip N] [--new-per-ip N] [--nofind-per-min N] [--room-hours H] [--room-mib M] [--link-kib K] [--wait-s S] [--idle-s S] [--stats S] [--quiet]";
    let mut ip = std::net::Ipv4Addr::UNSPECIFIED;
    let mut port = relay::DEFAULT_PORT;
    let mut cfg = relay::Config::default();
    let mut stats_s: u64 = 60;
    let mut quiet = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--host" => continue,
            "--no-udp" => {
                cfg.udp = false;
                continue;
            }
            "--quiet" => {
                quiet = true;
                continue;
            }
            _ => {}
        }
        let Some(v) = it.next() else {
            eprintln!("{} needs a value. {}", a, USAGE);
            return 2;
        };
        if a == "--listen" {
            let (h, p) = match v.split_once(':') {
                Some((h, p)) => (h, Some(p)),
                None => (v.as_str(), None),
            };
            match (h.parse::<std::net::Ipv4Addr>(), p.map(|p| p.parse::<u16>())) {
                (Ok(h), None) => ip = h,
                (Ok(h), Some(Ok(p))) => {
                    ip = h;
                    port = p;
                }
                _ => {
                    eprintln!("--listen {}: <IPv4>[:<port>] wanted", v);
                    return 2;
                }
            }
            continue;
        }
        let Ok(x) = v.parse::<u64>() else {
            eprintln!("{} {}: a number >= 0 wanted", a, v);
            return 2;
        };
        match a.as_str() {
            "--port" if x <= 65535 => port = x as u16,
            "--rooms" => cfg.max_rooms = x as usize,
            "--peers" => cfg.max_peers = x as usize,
            "--links" => cfg.max_conns = x as usize,
            "--per-ip" => cfg.max_per_ip = x.min(i32::MAX as u64) as i32,
            "--new-per-ip" => cfg.new_per_ip = x as i64,
            "--nofind-per-min" => cfg.nofind_per_min = x as i64,
            "--room-hours" => cfg.room_ms = x as i64 * 3600 * 1000,
            "--room-mib" => cfg.room_bytes = x << 20,
            "--link-kib" => cfg.link_bps = (x << 10) as i64,
            "--wait-s" => cfg.wait_ms = x as i64 * 1000,
            "--idle-s" => cfg.idle_ms = x as i64 * 1000,
            "--stats" => stats_s = x,
            _ => {
                eprintln!("unknown argument {}. {}", a, USAGE);
                return 2;
            }
        }
    }
    let log: Arc<dyn Fn(&str) + Send + Sync> = Arc::new(move |line: &str| {
        if !quiet {
            println!("{}", line)
        }
    });
    cfg.log = Some(log);
    let mut srv = match relay::RelayServer::start((ip, port), cfg, stats_s) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cannot listen on {}:{}: {}", ip, port, e);
            return 1;
        }
    };
    println!("Pokopia relay (pokopia-native coop-relay protocol) running on {}. Ctrl+C stops it.", srv.local_addr());
    println!("Anyone who can reach this address can open rooms here; rooms are authenticated end to end by the players, but their traffic is not encrypted.");
    for line in lan::log_lines(&lan::report(), srv.local_addr().port()) {
        println!("{}", line);
    }
    while !win_console::CTRL_C.load(Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
    srv.stop();
    0
}

/// A GUI-subsystem exe has no console: attach to the terminal that started it, and turn Ctrl+C into a clean stop.
mod win_console {
    use std::sync::atomic::AtomicBool;
    pub static CTRL_C: AtomicBool = AtomicBool::new(false);

    #[cfg(windows)]
    pub fn attach_and_hook_ctrl_c() {
        use std::sync::atomic::Ordering;
        const ATTACH_PARENT_PROCESS: u32 = u32::MAX;
        extern "system" {
            fn AttachConsole(pid: u32) -> i32;
            fn SetConsoleCtrlHandler(handler: Option<unsafe extern "system" fn(u32) -> i32>, add: i32) -> i32;
        }
        unsafe extern "system" fn on_ctrl(_kind: u32) -> i32 {
            CTRL_C.store(true, Ordering::SeqCst);
            1 // handled: main() stops the server and returns
        }
        // fails harmlessly when there is no parent console or the exe already has one (debug builds)
        unsafe {
            AttachConsole(ATTACH_PARENT_PROCESS);
            SetConsoleCtrlHandler(Some(on_ctrl), 1);
        }
    }
}

/// A handheld's logical screen is smaller than the default window: there the window starts maximized, which also keeps it clear of the
/// taskbar. On a larger screen the window opens where the player left it (`saved`), else at the default size from tauri.conf.json.
fn fit_window(window: &tauri::WebviewWindow, saved: Option<WindowPlace>) -> tauri::Result<()> {
    let Some(monitor) = window.current_monitor()? else { return Ok(()) };
    let scale = monitor.scale_factor();
    let screen = monitor.size().to_logical::<f64>(scale);
    if screen.width <= 1400.0 || screen.height < 800.0 {
        return window.maximize();
    }
    // the size the player left it at, if it still fits this screen (another monitor may be gone since)
    if let Some(p) = saved.filter(|p| p.width >= 900.0 && p.height >= 440.0 && p.width <= screen.width && p.height <= screen.height) {
        window.set_size(tauri::Size::Logical(tauri::LogicalSize::new(p.width, p.height)))?;
        // placed only where a monitor still is; Wayland ignores the position and the compositor places it
        let on_screen = window.available_monitors()?.iter().any(|m| {
            let (o, sz) = (m.position().to_logical::<f64>(m.scale_factor()), m.size().to_logical::<f64>(m.scale_factor()));
            p.x + 40.0 >= o.x && p.y + 20.0 >= o.y && p.x + 40.0 <= o.x + sz.width && p.y + 20.0 <= o.y + sz.height
        });
        if on_screen {
            window.set_position(tauri::Position::Logical(tauri::LogicalPosition::new(p.x, p.y)))?;
        }
    }
    Ok(())
}

/// The window's size and place as it closes, for the next start (fit_window). A maximized window keeps what was saved before.
fn place_of(window: &tauri::Window) -> Option<WindowPlace> {
    if window.is_maximized().unwrap_or(true) || window.is_minimized().unwrap_or(true) {
        return None;
    }
    let scale = window.scale_factor().ok()?;
    let size = window.inner_size().ok()?.to_logical::<f64>(scale);
    let pos = window.outer_position().ok()?.to_logical::<f64>(scale);
    // a closing window can report a degenerate size: never let it replace a good saved place
    (size.width >= 900.0 && size.height >= 440.0).then_some(WindowPlace { x: pos.x, y: pos.y, width: size.width, height: size.height })
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--host") {
        std::process::exit(run_headless(&args));
    }
    // `--play [--game <folder>]`: starts the game as Play does, without the window (a Steam or Sunshine shortcut). The game
    // folder is the launcher's own unless given: the one set in Settings lives in the window's config, not read here.
    if args.iter().any(|a| a == "--play") {
        #[cfg(windows)]
        win_console::attach_and_hook_ctrl_c();
        let game = args.iter().position(|a| a == "--game").and_then(|i| args.get(i + 1)).map(PathBuf::from).unwrap_or_else(default_game_dir);
        std::process::exit(play::run_cli(&game, &tools_dir()));
    }
    UPDATED.store(args.iter().any(|a| a == "--updated"), Ordering::SeqCst);
    // WebKitGTK's DMA-BUF renderer leaves the window blank or kills it ("Error 71 (Protocol error)") on NVIDIA's driver
    #[cfg(target_os = "linux")]
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() && Path::new("/sys/module/nvidia").exists() {
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(SetupState::default()))
        .manage(Arc::new(StorageState::default()))
        .manage(RelayState::default())
        .manage(RelayLease::default())
        .manage(Arc::new(update::UpdateState::default()))
        .manage(update::ReleaseStore::default())
        // a swap cut off by closing the window would have to wait for the next start to be undone
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                if window.state::<Arc<update::UpdateState>>().applying() {
                    api.prevent_close();
                    return;
                }
                if let Some(place) = place_of(window) {
                    let app = window.app_handle();
                    let mut cfg = load_config(app);
                    if cfg.window != Some(place) {
                        cfg.window = Some(place);
                        let _ = save_config(app.clone(), cfg);
                    }
                }
            }
        })
        .setup(|app| {
            if let Some(window) = app.get_webview_window("main") {
                // a failed screen query only means the window keeps its default size: it must never stop the launcher from starting
                let _ = fit_window(&window, load_config(app.handle()).window);
            }
            // Link Play starts from its defaults on every launcher start: nothing left from an earlier session decides the next one (#239)
            if let Err(e) = settings::reset_link_play(&game_dir_of(&load_config(app.handle()))) {
                eprintln!("Link Play reset: {e}");
            }
            // what the last update left: its folder, the old launcher (still exiting after a restart), a cut-off swap
            if let Some(launcher) = launcher_path() {
                let handle = app.handle().clone();
                let st = app.state::<Arc<update::UpdateState>>().inner().clone();
                st.set_cleaning(true);
                std::thread::spawn(move || {
                    let (root, game) = (install_dir(), game_dir_of(&load_config(&handle)));
                    update::clean_start(&root, &update::old_files(&launcher, &game));
                    st.set_cleaning(false);
                });
            }
            // Test runs drive the page without input: PKP_LAUNCHER_EVAL_FILE=<path>. A JS file written there is run in the page once and
            // deleted; it reports back through document.title (the window title). WebView2's DevTools port does not open for Tauri apps here.
            if let Ok(path) = std::env::var("PKP_LAUNCHER_EVAL_FILE") {
                if let Some(win) = app.get_webview_window("main") {
                    std::thread::spawn(move || loop {
                        std::thread::sleep(std::time::Duration::from_millis(300));
                        if let Ok(js) = std::fs::read_to_string(&path) {
                            let _ = std::fs::remove_file(&path);
                            let _ = win.eval(&js);
                        }
                    });
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            test_report,
            get_config,
            launcher_state,
            get_releases,
            install_update,
            list_builds,
            install_version,
            cancel_update,
            open_release_url,
            save_config,
            get_overview,
            pick_folder,
            validate_dump,
            pick_nup,
            pick_nsp,
            import_dlc,
            import_nup,
            cancel_nup,
            save_dump,
            read_settings,
            write_settings,
            play,
            setup_status,
            start_setup,
            preflight,
            open_setup_log,
            cancel_setup,
            shader_storage,
            compact_shaders,
            expand_shaders,
            cancel_shader_storage,
            dlss_status,
            fetch_dlss,
            game_status,
            save_sources,
            import_save,
            open_save_dir,
            profiles_list,
            profile_add,
            profile_rename,
            profile_remove,
            profile_select,
            repair_plan,
            repair_apply,
            repair_undo,
            repair_last,
            backup_status,
            backup_list,
            backup_now,
            backup_restore,
            open_backup_dir,
            open_album_dir,
            steam_status,
            steam_add,
            steam_remove,
            open_game_file,
            open_game_dir,
            report_problem,
            icloud_status,
            open_icloud_dir,
            relay_start,
            relay_stop,
            relay_status,
            lan_report,
            public_ip,
            room_code_new,
            room_code_check,
            room_code_host,
            port_forward_status,
            port_forward_set,
            cheats_themes::cheats_themes,
            cheats_themes::cheats_theme_import,
            cheats_themes::cheats_themes_open,
            cheats_fetch::cheats_info,
            cheats_fetch::cheats_fetch,
            cheats_fetch::cheats_dir_open,
            mods::mods_list,
            mods::mods_install,
            mods::mods_change,
            mods::mods_make,
            mods::mods_pack,
            mods::mods_pick,
            mods::mods_open,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
