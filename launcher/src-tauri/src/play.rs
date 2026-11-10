// SPDX-License-Identifier: GPL-3.0-or-later
//! Starts pkp.exe, watches it, and reports how it ended. On macOS the game is the native `pkp` (host::GAME_EXE); a crash ends it with
//! a signal rather than an exit code, and the message names the signal.

use serde::Serialize;
use std::path::Path;
use std::sync::Mutex;
use std::time::{Duration, SystemTime};
use tauri::{AppHandle, Emitter, Manager};

/// Files the launcher may open for the user, by name, inside the game folder.
pub const REPORT_FILES: [&str; 4] = ["pkp_crash.txt", "pkp_freeze.txt", "pkp_fatal.txt", "pkp.log"];

#[derive(Serialize, Clone, Default)]
pub struct ReportFile {
    pub name: String,
    /// Written since this run started.
    pub fresh: bool,
}

#[derive(Serialize, Clone)]
pub struct Status {
    /// "idle", "running" or "exited"
    pub state: String,
    pub code: Option<i32>,
    pub crashed: bool,
    pub files: Vec<ReportFile>,
    pub message: String,
}

impl Default for Status {
    fn default() -> Self {
        Status { state: "idle".into(), code: None, crashed: false, files: vec![], message: String::new() }
    }
}

#[derive(Default)]
pub struct PlayState(pub Mutex<Status>);

/// While the game runs (macOS): every few seconds the shaders it built since the last look are translated (shaders.rs translate_live),
/// so what they draw shows up in this session, not only after a restart. finish() stops it and waits for a translation under way, so
/// the translation after the game ends (translate_missing) never runs beside it.
struct LiveShaders {
    stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl LiveShaders {
    const EVERY: Duration = Duration::from_secs(4);

    fn start(dir: &Path, tools: &Path) -> Self {
        use std::sync::atomic::Ordering;
        let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        if !cfg!(target_os = "macos") {
            return LiveShaders { stop, thread: None };
        }
        let (s, d, t) = (stop.clone(), dir.to_path_buf(), tools.to_path_buf());
        let thread = std::thread::spawn(move || loop {
            let until = std::time::Instant::now() + Self::EVERY;
            while std::time::Instant::now() < until {
                if s.load(Ordering::SeqCst) {
                    return;
                }
                std::thread::sleep(Duration::from_millis(200));
            }
            if let Err(e) = crate::shaders::translate_live(&d, &t) {
                eprintln!("shaders during play: {}", e);
            }
        });
        LiveShaders { stop, thread: Some(thread) }
    }

    fn finish(mut self) {
        self.stop.store(true, std::sync::atomic::Ordering::SeqCst);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

fn set_status(app: &AppHandle, s: Status) {
    *app.state::<PlayState>().0.lock().unwrap() = s.clone();
    let _ = app.emit("game-status", s);
}

fn report_files(dir: &Path, started: SystemTime) -> Vec<ReportFile> {
    REPORT_FILES
        .iter()
        .filter_map(|n| {
            let md = std::fs::metadata(dir.join(n)).ok()?;
            let fresh = md.modified().map(|m| m + Duration::from_secs(2) >= started).unwrap_or(false);
            Some(ReportFile { name: n.to_string(), fresh })
        })
        .collect()
}

pub fn start(app: &AppHandle, game_dir: &Path, minimize: bool) -> Result<(), String> {
    {
        let st = app.state::<PlayState>();
        if st.0.lock().unwrap().state == "running" {
            return Err("The game is already running.".into());
        }
    }
    let game = crate::host::GAME_EXE;
    let exe = game_dir.join(game);
    if !exe.is_file() {
        return Err(format!("{0} was not found in {1}. Put the launcher next to {0} or set the game folder in Settings.", game, crate::dump::fwd(game_dir)));
    }

    let app2 = app.clone();
    let dir = game_dir.to_path_buf();
    let tools = crate::tools_dir();
    // launcher.json: the iCloud Drive switch and its last sync (icloud.rs), automatic port forwarding (portmap.rs)
    let cfg_file = crate::config_path(app).ok();
    let first = if crate::shaders::needs_warm_up(game_dir) {
        "Collecting the shaders the game builds while it runs (once, about a minute)..."
    } else {
        "Checking the shaders the game builds while it runs..."
    };
    set_status(app, Status { state: "running".into(), message: first.into(), ..Default::default() });
    std::thread::spawn(move || {
        // every start: the run-time shaders seen so far must all be translated (the warm-up run collects them the first
        // time, e.g. for a setup finished before it existed). A failure here is logged in the status, the game still starts.
        let mut prep = match crate::shaders::prepare(&dir, &tools, &std::sync::atomic::AtomicBool::new(false)) {
            Ok(Some(n)) if n > 0 => format!(" {} shaders were translated before the start.", n),
            Err(e) => format!(" Shaders were not prepared: {}.", e),
            _ => String::new(),
        };
        // macOS: the key file's pipelines that are not compiled yet (the ones the last session recorded, an update that brought a bigger
        // seed, or ones that waited for a shader the last run translated); a quick check when there are none (shaders.rs prepare_pipelines)
        if let Err(e) = crate::shaders::prepare_pipelines(&dir, &tools, &std::sync::atomic::AtomicBool::new(false), None, |done, total| {
            set_status(&app2, Status { state: "running".into(), message: format!("Compiling the game's graphics for Metal: {} of {}...", done, total), ..Default::default() });
        }) {
            prep.push_str(&format!(" Graphics were not compiled ahead: {}.", e));
        }
        // iCloud Drive, when the player switched it on: a newer save from another Mac is brought in before the game opens it, the save
        // here backed up first (icloud.rs before_play). It goes into the status line; a conflict or a problem also gets a message.
        if let Some(cfg) = cfg_file.as_deref() {
            if crate::icloud::is_on(cfg) {
                set_status(&app2, Status { state: "running".into(), message: "Checking your save in iCloud Drive...".into(), ..Default::default() });
            }
            if let Some(n) = crate::icloud::before_play(&dir, cfg) {
                prep.push_str(&format!(" {}", n.text));
                if n.important {
                    let _ = app2.emit("save-sync", &n.text);
                }
            }
        }
        // Calculate after shader preparation, immediately before the process starts, so a long preparation cannot age the offset.
        let mut cmd = match crate::events::override_for(&dir).and_then(|clock| game_command(&exe, &dir, clock)) {
            Ok(c) => c,
            Err(e) => {
                set_status(&app2, Status { state: "exited".into(), crashed: true, message: format!("Could not start {}: {}", game, e), ..Default::default() });
                return;
            }
        };
        let started = SystemTime::now();
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                set_status(&app2, Status { state: "exited".into(), crashed: true, message: format!("Could not start {}: {}", game, e), ..Default::default() });
                return;
            }
        };
        // the page tells this moment by "<game> started" (action.js gamePhase)
        set_status(&app2, Status { state: "running".into(), message: format!("{} started (process {})", game, child.id()), ..Default::default() });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.minimize();
            }
        }
        // the player's save backups: a timer while the game runs (when switched on), one more copy once it has ended
        let backups = crate::savebackup::Watch::start(&dir);
        // hosting over the internet: the Link Play port is opened on the router while the game runs, on a thread of its own (portmap.rs)
        let ports = crate::portmap::for_game(&dir, cfg_file.as_deref());
        let live = LiveShaders::start(&dir, &tools);
        let result = child.wait();
        live.finish();
        backups.finish();
        if let Some(p) = ports {
            p.finish(Duration::from_secs(3));
        }
        // the save as the game left it goes up to iCloud Drive (icloud.rs after_play), after the closing backup
        let synced = match cfg_file.as_deref().and_then(|cfg| crate::icloud::after_play(&dir, cfg)) {
            Some(n) => {
                if n.important {
                    let _ = app2.emit("save-sync", &n.text);
                }
                format!(" {}", n.text)
            }
            None => String::new(),
        };
        // macOS: a crash (SIGSEGV, SIGBUS, an abort) ends the native game with a signal and no exit code
        #[cfg(target_os = "macos")]
        let signal = result.as_ref().ok().and_then(std::os::unix::process::ExitStatusExt::signal);
        #[cfg(not(target_os = "macos"))]
        let signal: Option<i32> = None;
        let (code, wait_err) = match result {
            Ok(s) => (s.code(), None),
            Err(e) => (None, Some(e.to_string())),
        };
        // shaders the game built while it ran: translated now, so the next run has them (the state stays "running" meanwhile)
        set_status(&app2, Status { state: "running".into(), message: "Translating the shaders the game built while it ran...".into(), ..Default::default() });
        let shaders = match crate::shaders::translate_missing(&dir, &tools) {
            Ok(0) => String::new(),
            Ok(n) => format!(" {} new shaders translated.", n),
            Err(e) => format!(" New shaders were not translated: {}.", e),
        };
        let files = report_files(&dir, started);
        let crash_file = files.iter().any(|f| f.fresh && f.name != "pkp.log");
        let crashed = crash_file || code.map_or(true, |c| c != 0);
        let message = match (&wait_err, code, signal) {
            (Some(e), _, _) => format!("Lost track of {}: {}", game, e),
            (None, Some(0), _) => format!("{} exited normally.", game),
            (None, Some(c), _) => format!("{} exited with code {} (0x{:08X}).", game, c, c as u32),
            (None, None, Some(s)) => format!("{} was ended by signal {} ({}).", game, s, signal_name(s)),
            (None, None, None) => format!("{} was ended by the system.", game),
        };
        set_status(&app2, Status { state: "exited".into(), code, crashed, files, message: message + &prep + &shaders + &synced });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }
    });
    Ok(())
}

/// How Play starts pkp.exe: in its folder, through Proton on Linux (host.rs), natively on macOS. Every path (exefs, romfs, shader
/// cache) comes from pkp.ini next to it. The run-time files beside it are brought up to date first (macOS: the app's runtime/ folder,
/// Vulkan included; the runtime picks the Vulkan driver itself, so VK_DRIVER_FILES is left to the player's environment).
fn game_command(exe: &Path, dir: &Path, clock: Option<crate::events::ClockOverride>) -> Result<std::process::Command, String> {
    if let Some(src) = crate::host::runtime_source(&crate::tools_dir()) {
        crate::host::sync_runtime_files(&src, dir)?;
    }
    let mut cmd = crate::host::windows_program(exe)?;
    cmd.current_dir(dir).args(["--log", "pkp.log"]);
    if let Some(clock) = clock {
        clock.apply_to(&mut cmd);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no stray console if pkp.exe is a console build
    }
    Ok(cmd)
}

/// `--play`: what Play does, without the window, for a shortcut that starts the game directly (Steam, Sunshine). Checks
/// that setup finished, prepares the run-time shaders, runs pkp.exe until it exits, then translates the shaders it built.
/// Returns the game's exit code (1 when it could not start; macOS: 128 + the signal that ended it, as a shell reports it).
pub fn run_cli(game_dir: &Path, tools: &Path) -> i32 {
    let fail = |m: String| {
        eprintln!("{}", m);
        1
    };
    let game = crate::host::GAME_EXE;
    let exe = game_dir.join(game);
    if !exe.is_file() || (crate::gamebuild::is_package_b(tools) && !crate::gamebuild::is_built(game_dir, tools)) {
        return fail(format!("{} in {} is missing or from an older release: open the launcher and press Set up.", game, crate::dump::fwd(game_dir)));
    }
    if !crate::shaders::is_complete(game_dir) || !game_dir.join(crate::settings::PKP_INI).is_file() {
        return fail("Setup has not finished: open the launcher and press Set up.".into());
    }
    match crate::shaders::prepare(game_dir, tools, &std::sync::atomic::AtomicBool::new(false)) {
        Ok(Some(n)) if n > 0 => println!("{} shaders were translated before the start.", n),
        Err(e) => eprintln!("Shaders were not prepared: {}.", e),
        _ => {}
    }
    if let Err(e) = crate::shaders::prepare_pipelines(game_dir, tools, &std::sync::atomic::AtomicBool::new(false), None, |_, _| {}) {
        eprintln!("Graphics were not compiled ahead: {}.", e);
    }
    // the same rule as Play in the window: without the active profile in pkp.ini the game could start on another player's account and save
    if let Err(e) = crate::profiles::apply_active(game_dir) {
        return fail(format!("The profile could not be applied, so the game was not started: {}", e));
    }
    // the same iCloud Drive save sync and router port as Play in the window (icloud.rs, portmap.rs); launcher.json is found without it
    let cfg_file = crate::cli_config_path();
    if let Some(n) = cfg_file.as_deref().and_then(|cfg| crate::icloud::before_play(game_dir, cfg)) {
        println!("{}", n.text);
    }
    let clock = match crate::events::override_for(game_dir) {
        Ok(clock) => clock,
        Err(e) => return fail(e),
    };
    let backups = crate::savebackup::Watch::start(game_dir);   // the same save backups as Play in the window
    let ports = crate::portmap::for_game(game_dir, cfg_file.as_deref());
    let live = LiveShaders::start(game_dir, tools);
    let status = game_command(&exe, game_dir, clock).and_then(|mut c| c.status().map_err(|e| format!("Could not start {}: {}", game, e)));
    live.finish();
    backups.finish();
    if let Some(p) = ports {
        p.finish(Duration::from_secs(3));
    }
    if let Some(n) = cfg_file.as_deref().and_then(|cfg| crate::icloud::after_play(game_dir, cfg)) {
        println!("{}", n.text);
    }
    let code = match status {
        #[cfg(target_os = "macos")]
        Ok(st) => st.code().unwrap_or_else(|| {
            let s = std::os::unix::process::ExitStatusExt::signal(&st).unwrap_or(0);
            eprintln!("{} was ended by signal {} ({}).", game, s, signal_name(s));
            128 + s
        }),
        #[cfg(not(target_os = "macos"))]
        Ok(st) => st.code().unwrap_or(1),
        Err(e) => return fail(e),
    };
    match crate::shaders::translate_missing(game_dir, tools) {
        Ok(n) if n > 0 => println!("{} new shaders translated.", n),
        Err(e) => eprintln!("New shaders were not translated: {}.", e),
        _ => {}
    }
    code
}

/// The usual name of a signal that ends a crashed or stopped program (macOS numbers), for the message after the game ends.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn signal_name(s: i32) -> &'static str {
    match s {
        4 => "SIGILL, illegal instruction",
        5 => "SIGTRAP, a trap or a failed check",
        6 => "SIGABRT, aborted",
        8 => "SIGFPE, arithmetic error",
        9 => "SIGKILL, killed",
        10 => "SIGBUS, bad memory access",
        11 => "SIGSEGV, bad memory access",
        13 => "SIGPIPE",
        15 => "SIGTERM, asked to end",
        _ => "a signal",
    }
}

pub fn open_with_shell(path: &Path) -> Result<(), String> {
    // explorer (Linux: xdg-open, macOS: open) opens a file in its default app and a folder in a window
    crate::host::open(path.as_os_str())
}

/// Opens an http(s) link in the default browser (same route as open_with_shell).
pub fn open_url(url: &str) -> Result<(), String> {
    crate::host::open(std::ffi::OsStr::new(url))
}
