// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
//! What the launcher does differently on Linux (SteamOS, the Steam Deck, Bazzite, ...), where the game stays a Windows program,
//! and on macOS, where it is a native one.
//!
//! On Windows the release's Windows tools run and pkp.exe starts directly. On Linux the launcher is a native program (Tauri
//! draws it with WebKitGTK, so it needs no WebView2), the tools that only prepare files run as Linux programs from
//! `tools/linux`: Python 3.13 with lz4 and capstone (pkp_build.py, the lifter, extract_corpus.py) and Zig 0.17.0, the version
//! the release bundles for Windows, which cross-compiles the same pkp.exe. `prepare_tools` fetches them once, each pinned by
//! size and SHA-256. The two Windows programs, the shader translator and pkp.exe, run through `runner`: Steam's Proton inside
//! the Steam Linux Runtime, as Steam starts a game (PKP_PROTON picks another Proton folder), else umu-run, else Wine. There
//! the Linux root is drive Z:, so paths handed to them go through `win_path`.
//! Steam's Proton is found as tinfoil-chewer/pokopia-recomp-linux (GPL-3.0) linux/pkp_linux.py `find_proton` finds it.
//!
//! macOS (Apple silicon) is a native port, not Wine: the runtime is `libpkp_rt.dylib`, pkp_build.py links the game as the arm64
//! program `pkp`, and the shader translator is a self-contained osx-arm64 .NET publish, so nothing runs through a runner and paths
//! stay as they are. The launcher is `PokoMac.app`, which must stay as it was signed: its read-only payload sits in
//! `Contents/Resources` (`tools/` as on Windows, and `runtime/`, the files the game needs next to it: the runtime, the Vulkan loader
//! with the KosmicKrisp and MoltenVK drivers and their manifests, the fonts), and everything written lives outside the bundle, in
//! `~/Library/Application Support/PokoMac` (`support_dir`): the default game folder, and `tools/macos` with the Python 3.13
//! (python-build-standalone, aarch64-apple-darwin) and Zig 0.17.0 (aarch64-macos) that `prepare_tools` fetches, pinned as on Linux.
//! `sync_runtime_files` copies `runtime/` into the game folder before the game runs, so an updated app refreshes it.
//! No crate-local imports, so the examples can build this file alone next to gamebuild.rs and shaders.rs.

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::atomic::AtomicBool;

/// The game program Set up builds into the game folder: the Windows `pkp.exe` (run through Proton on Linux), the native `pkp` on
/// macOS (pkp_build.py `EXE`).
pub const GAME_EXE: &str = if cfg!(target_os = "macos") { "pkp" } else { "pkp.exe" };
/// The runtime the game loads from its own folder (pkp_build.py links against its import library or, on macOS, the dylib itself).
pub const RUNTIME_LIB: &str = if cfg!(target_os = "macos") { "libpkp_rt.dylib" } else { "pkp_rt.dll" };
/// The shader translator in `tools/spirv_harness`: the win-x64 publish (Proton on Linux), the osx-arm64 one on macOS.
pub const HARNESS_EXE: &str = if cfg!(target_os = "macos") { "spirv_harness" } else { "spirv_harness.exe" };

/// The Python that runs the release's scripts (pkp_build.py, extract_corpus.py, fetch_dlss.py).
pub fn python(tools_dir: &Path) -> PathBuf {
    if cfg!(windows) {
        tools_dir.join("python").join("python.exe")
    } else {
        native_tools(tools_dir).join("python").join("bin").join("python3")
    }
}

/// The Zig pkp_build.py compiles and links with (the script finds the same path on its own on Windows and Linux; on macOS the
/// launcher hands it over as PKP_ZIG, gamebuild.rs).
pub fn zig(tools_dir: &Path) -> PathBuf {
    if cfg!(windows) {
        tools_dir.join("build").join("zig").join("zig.exe")
    } else {
        native_tools(tools_dir).join("zig").join("zig")
    }
}

/// Where `prepare_tools` keeps the downloaded Python and Zig: `tools/linux`. macOS: `tools/macos`, except for the `tools/` of the
/// .app bundle (Contents/Resources/tools), which is never written to: there they go to `support_dir()/tools/macos`.
fn native_tools(tools_dir: &Path) -> PathBuf {
    #[cfg(target_os = "macos")]
    if bundle_of(tools_dir).is_some() {
        return support_dir().join("tools").join("macos");
    }
    tools_dir.join(if cfg!(target_os = "macos") { "macos" } else { "linux" })
}

/// macOS: `~/Library/Application Support/PokoMac`, the launcher's writable folder: the default game folder, and the downloaded
/// build tools (tools/macos) when the launcher runs from its .app bundle.
#[cfg(target_os = "macos")]
pub fn support_dir() -> PathBuf {
    home().join("Library").join("Application Support").join("PokoMac")
}

/// macOS: the .app bundle `p` lies in (the nearest `<name>.app` above it with a Contents/Info.plist), if any.
#[cfg(target_os = "macos")]
pub fn bundle_of(p: &Path) -> Option<PathBuf> {
    p.ancestors().find(|a| a.extension().is_some_and(|e| e == "app") && a.join("Contents").join("Info.plist").is_file()).map(Path::to_path_buf)
}

/// The folder the game's run-time files are copied from into a game folder of its own (`sync_runtime_files`): the install, which is
/// where `tools/` sits. macOS: `runtime/` beside `tools/` (Contents/Resources/runtime in the app).
pub fn runtime_source(tools_dir: &Path) -> Option<PathBuf> {
    let install = tools_dir.parent()?;
    Some(if cfg!(target_os = "macos") { install.join("runtime") } else { install.to_path_buf() })
}

/// A command for a program of this system (Python, tar, Proton, xdg-open). In an AppImage, whose runtime points
/// variables such as GTK_PATH, GIO_EXTRA_MODULES or XDG_DATA_DIRS into the AppImage for the launcher's own libraries, the
/// program gets the environment without those entries (and without APPDIR, APPIMAGE, ARGV0, OWD); elsewhere nothing
/// changes. After tinfoil-chewer/pokopia-recomp-linux (GPL-3.0) launcher/src-tauri/src/linux.rs `host_env`.
pub fn command<S: AsRef<std::ffi::OsStr>>(program: S) -> Command {
    let mut cmd = Command::new(program);
    let appdir = std::env::var("APPDIR").unwrap_or_default();
    let vars = std::env::vars_os().filter_map(|(k, v)| Some((k.into_string().ok()?, v.into_string().ok()?)));
    for (k, v) in appimage_env_fixes(&appdir, vars) {
        match v {
            Some(v) => cmd.env(k, v),
            None => cmd.env_remove(k),
        };
    }
    cmd
}

/// What `command` changes for `appdir` (empty: not an AppImage, nothing): (variable, Some(new value) or None = removed).
fn appimage_env_fixes(appdir: &str, vars: impl Iterator<Item = (String, String)>) -> Vec<(String, Option<String>)> {
    let appdir = appdir.trim_end_matches('/');
    if appdir.is_empty() {
        return Vec::new();
    }
    let mut out = Vec::new();
    for (k, v) in vars {
        if matches!(k.as_str(), "APPDIR" | "APPIMAGE" | "ARGV0" | "OWD") {
            out.push((k, None));
        } else if v.contains(appdir) {
            // a list keeps its other entries; a single path into the AppImage goes
            let kept: Vec<&str> = v.split(':').filter(|e| !e.is_empty() && !e.contains(appdir)).collect();
            out.push((k, (v.contains(':') && !kept.is_empty()).then(|| kept.join(":"))));
        }
    }
    out
}

/// A command that runs one of the game's programs, `exe` (pkp, the shader translator): Windows programs, run as they are on Windows
/// and through `runner` on Linux; on macOS native builds, run as they are.
pub fn windows_program(exe: &Path) -> Result<Command, String> {
    #[cfg(windows)]
    {
        Ok(Command::new(exe))
    }
    #[cfg(target_os = "macos")]
    {
        Ok(command(exe))
    }
    #[cfg(not(any(windows, target_os = "macos")))]
    {
        let r = runner()?;
        let mut cmd = command(&r.argv[0]);
        cmd.args(&r.argv[1..]).arg(exe);
        for (k, v) in &r.env {
            cmd.env(k, v);
        }
        Ok(cmd)
    }
}

/// `p` as the game's programs see it: unchanged on Windows and macOS, `Z:\...` under Proton and Wine (Z: is the Linux root there).
pub fn win_path(p: &Path) -> OsString {
    #[cfg(any(windows, target_os = "macos"))]
    {
        p.as_os_str().to_os_string()
    }
    #[cfg(not(any(windows, target_os = "macos")))]
    {
        let abs = if p.is_absolute() { p.to_path_buf() } else { std::env::current_dir().map(|d| d.join(p)).unwrap_or_else(|_| p.to_path_buf()) };
        OsString::from(format!("Z:{}", abs.to_string_lossy().replace('/', "\\")))
    }
}

/// Stops a tool and what it started: on Linux and macOS the compilers, or Proton's wine processes (`lower_priority` made it the
/// leader of its own process group).
pub fn kill(child: &mut Child) {
    #[cfg(not(windows))]
    unsafe {
        libc::kill(-(child.id() as i32), libc::SIGKILL);
    }
    let _ = child.kill();
}

/// Linux and macOS: the tool runs niced (10, idle 19; its children inherit it) in a process group of its own, for `kill`.
#[cfg(not(windows))]
pub fn lower_priority(cmd: &mut Command, idle: bool) {
    use std::os::unix::process::CommandExt;
    let n = if idle { 19 } else { 10 };
    cmd.process_group(0);
    // SAFETY: nice(2) is async-signal-safe, which is all pre_exec asks of the closure
    unsafe {
        cmd.pre_exec(move || {
            libc::nice(n);
            Ok(())
        });
    }
}

/// Free bytes on the file system that holds `dir` (or its nearest existing parent).
#[cfg(not(windows))]
pub fn free_bytes(dir: &Path) -> Option<u64> {
    use std::os::unix::ffi::OsStrExt;
    let mut d = dir;
    while !d.exists() {
        d = d.parent()?;
    }
    let c = std::ffi::CString::new(d.as_os_str().as_bytes()).ok()?;
    // SAFETY: c is NUL-terminated and s is a valid out pointer for the call
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    (unsafe { libc::statvfs(c.as_ptr(), &mut s) } == 0).then(|| s.f_bavail as u64 * s.f_frsize as u64)
}

/// Opens a file in its default app, a folder in the file manager (Finder on macOS), or a link in the browser.
pub fn open(target: &std::ffi::OsStr) -> Result<(), String> {
    let opener = if cfg!(windows) {
        "explorer.exe"
    } else if cfg!(target_os = "macos") {
        "/usr/bin/open"
    } else {
        "xdg-open"
    };
    let mut child = command(opener).arg(target).spawn().map_err(|e| format!("Could not open {}: {}", target.to_string_lossy(), e))?;
    // reaped, so no exited opener lingers as a zombie while the launcher runs
    std::thread::spawn(move || child.wait());
    Ok(())
}

/// How the Windows programs run on Linux: `argv` before the program's path, extra environment, and a name for the setup
/// log and the checks before Set up.
#[cfg(not(any(windows, target_os = "macos")))]
pub struct Runner {
    pub argv: Vec<OsString>,
    pub env: Vec<(&'static str, OsString)>,
    pub label: String,
}

/// `$XDG_DATA_HOME/pokomac` (~/.local/share/pokomac): the Proton and Wine prefixes, kept out of the game folder.
#[cfg(not(any(windows, target_os = "macos")))]
fn data_dir() -> PathBuf {
    std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".local").join("share"))
        .join("pokomac")
}

#[cfg(not(windows))]
fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/"))
}

/// macOS: a process named `name` runs (its short name, as Activity Monitor and `ps -c` show it: `pkp`, `steam_osx`). macOS has no
/// /proc: libproc's process list and names, the same calls `pgrep` makes.
#[cfg(target_os = "macos")]
pub fn process_running(name: &str) -> bool {
    // SAFETY: proc_listallpids fills at most the given byte count with pids; proc_name writes at most the buffer's size
    unsafe {
        let n = libc::proc_listallpids(std::ptr::null_mut(), 0);
        if n <= 0 {
            return false;
        }
        let mut pids = vec![0 as libc::pid_t; n as usize + 64];
        let bytes = (pids.len() * std::mem::size_of::<libc::pid_t>()) as libc::c_int;
        let got = libc::proc_listallpids(pids.as_mut_ptr().cast(), bytes).max(0) as usize;
        let mut buf = [0u8; 256];
        pids[..got.min(pids.len())].iter().any(|&pid| {
            let len = libc::proc_name(pid, buf.as_mut_ptr().cast(), buf.len() as u32);
            len > 0 && &buf[..len as usize] == name.as_bytes()
        })
    }
}

#[cfg(not(any(windows, target_os = "macos")))]
fn which(exe: &str) -> Option<PathBuf> {
    std::env::var_os("PATH").and_then(|p| std::env::split_paths(&p).map(|d| d.join(exe)).find(|f| f.is_file()))
}

/// The GPU driver's shader caches, set up as Steam sets them up for a game it starts: a folder of the game's own that the
/// driver never trims. The shared default (~/.cache/nvidia, ~/.cache/mesa_shader_cache) is trimmed to a size limit by every
/// program using it, and pipelines trimmed away are compiled again at the draw (20-40 ms each). A variable already set wins.
#[cfg(not(any(windows, target_os = "macos")))]
fn shader_cache_env() -> Vec<(&'static str, OsString)> {
    let dir = data_dir().join("shadercache");
    let (nvidia, mesa) = (dir.join("nvidia"), dir.join("mesa"));
    let _ = std::fs::create_dir_all(&nvidia);
    let _ = std::fs::create_dir_all(&mesa);
    [
        ("__GL_SHADER_DISK_CACHE_PATH", nvidia.into_os_string()),
        ("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1".into()),
        ("MESA_SHADER_CACHE_DIR", mesa.into_os_string()),
        ("MESA_SHADER_CACHE_MAX_SIZE", "10G".into()),
    ]
    .into_iter()
    .filter(|(k, _)| std::env::var_os(k).is_none())
    .collect()
}

/// Steam's Proton, else umu-run, else Wine, each with `shader_cache_env`. Err names what to install.
#[cfg(not(any(windows, target_os = "macos")))]
pub fn runner() -> Result<Runner, String> {
    let mut r = find_runner()?;
    r.env.extend(shader_cache_env());
    Ok(r)
}

#[cfg(not(any(windows, target_os = "macos")))]
fn find_runner() -> Result<Runner, String> {
    if let Some(r) = steam_proton() {
        return Ok(r);
    }
    let prefix = |name: &str| {
        let p = data_dir().join(name);
        let _ = std::fs::create_dir_all(&p);
        p.into_os_string()
    };
    if let Some(umu) = which("umu-run") {
        // umu picks (and fetches) its own Proton unless PROTONPATH says which
        return Ok(Runner { argv: vec![umu.into()], env: vec![("WINEPREFIX", prefix("umu")), ("GAMEID", "0".into())], label: "umu-run".into() });
    }
    if let Some(wine) = which("wine") {
        // mscoree stays on: Wine needs it to load the shader translator's .NET assemblies
        let env = vec![("WINEPREFIX", prefix("wine")), ("WINEDEBUG", "-all".into()), ("WINEDLLOVERRIDES", "mshtml=".into())];
        return Ok(Runner { argv: vec![wine.into()], env, label: "Wine".into() });
    }
    Err("Nothing to run the game's Windows programs with was found. Install Steam and its Proton (in Steam: Library, Tools, Proton Experimental), umu-launcher, or Wine.".into())
}

/// The values of `"key" "value"` lines in a Valve KeyValues file (libraryfolders.vdf, toolmanifest.vdf, appmanifest_*.acf).
#[cfg(not(any(windows, target_os = "macos")))]
fn vdf_values(text: &str, key: &str) -> Vec<String> {
    let quoted = format!("\"{}\"", key);
    text.lines()
        .filter_map(|l| l.trim().strip_prefix(quoted.as_str()))
        .filter_map(|rest| rest.trim().strip_prefix('"').and_then(|v| v.strip_suffix('"')))
        .map(|v| v.replace("\\\\", "\\"))
        .collect()
}

/// (Steam root, its library folders) from libraryfolders.vdf. None without Steam.
#[cfg(not(any(windows, target_os = "macos")))]
fn steam_libraries() -> Option<(PathBuf, Vec<PathBuf>)> {
    for r in [".local/share/Steam", ".steam/steam", ".steam/root", ".var/app/com.valvesoftware.Steam/.local/share/Steam"] {
        let Ok(root) = std::fs::canonicalize(home().join(r)) else { continue };
        let Ok(text) = std::fs::read_to_string(root.join("steamapps").join("libraryfolders.vdf")) else { continue };
        let mut libs: Vec<PathBuf> = Vec::new();
        for lib in std::iter::once(root.clone()).chain(vdf_values(&text, "path").into_iter().map(PathBuf::from)) {
            let Ok(lib) = std::fs::canonicalize(lib) else { continue };
            if lib.join("steamapps").is_dir() && !libs.contains(&lib) {
                libs.push(lib);
            }
        }
        return Some((root, libs));
    }
    None
}

/// PKP_PROTON (a folder with `proton`), else Proton Experimental, else the newest "Proton N.N" Steam installed.
#[cfg(not(any(windows, target_os = "macos")))]
fn pick_proton(libs: &[PathBuf]) -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("PKP_PROTON").map(PathBuf::from) {
        return p.join("proton").is_file().then_some(p);
    }
    let rank = |name: &str| -> (u8, Vec<u32>) {
        if name.contains("Experimental") {
            return (2, vec![]);
        }
        let nums: Vec<u32> = name.split(|c: char| !c.is_ascii_digit()).filter_map(|x| x.parse().ok()).collect();
        (if nums.is_empty() { 0 } else { 1 }, nums)
    };
    let mut found: Vec<(String, PathBuf)> = Vec::new();
    for lib in libs {
        let Ok(rd) = std::fs::read_dir(lib.join("steamapps").join("common")) else { continue };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            if name.starts_with("Proton") && e.path().join("proton").is_file() {
                found.push((name, e.path()));
            }
        }
    }
    found.into_iter().max_by_key(|(n, _)| rank(n)).map(|(_, p)| p)
}

/// Steam's Proton, inside the Steam Linux Runtime container its toolmanifest.vdf asks for when that is installed (as Steam
/// starts a game). Its prefix is `data_dir()/proton` (STEAM_COMPAT_DATA_PATH).
#[cfg(not(any(windows, target_os = "macos")))]
fn steam_proton() -> Option<Runner> {
    let (root, libs) = steam_libraries()?;
    let pdir = pick_proton(&libs)?;
    let mut argv: Vec<OsString> = vec![pdir.join("proton").into(), "waitforexitandrun".into()];
    let mut label = pdir.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_else(|| "Proton".into());
    let manifest = std::fs::read_to_string(pdir.join("toolmanifest.vdf")).unwrap_or_default();
    if let Some(appid) = vdf_values(&manifest, "require_tool_appid").first() {
        for lib in &libs {
            let acf = std::fs::read_to_string(lib.join("steamapps").join(format!("appmanifest_{}.acf", appid))).unwrap_or_default();
            let Some(dir) = vdf_values(&acf, "installdir").into_iter().next() else { continue };
            let entry = lib.join("steamapps").join("common").join(&dir).join("_v2-entry-point");
            if entry.is_file() {
                argv.splice(0..0, [entry.into_os_string(), "--verb=waitforexitandrun".into(), "--".into()]);
                label = format!("{} in {}", label, dir);
                break;
            }
        }
    }
    let data = data_dir().join("proton");
    let _ = std::fs::create_dir_all(&data);
    Some(Runner { argv, env: vec![("STEAM_COMPAT_DATA_PATH", data.into()), ("STEAM_COMPAT_CLIENT_INSTALL_PATH", root.into())], label })
}

/// One download of `prepare_tools`, checked against the size and SHA-256 its publisher lists for it.
struct Pinned {
    file: &'static str,
    url: &'static str,
    size: u64,
    sha256: &'static str,
}

// python-build-standalone 20261013 (its SHA256SUMS), the PyPI wheels the Windows release bundles, Zig 0.17.0 (ziglang.org
// download index). Bump together with the Windows release's tools/python and tools/build/zig.
#[cfg(not(target_os = "macos"))]
const KIT: &str = "Linux";
#[cfg(not(target_os = "macos"))]
const PYTHON: Pinned = Pinned {
    file: "cpython-3.13.16+20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
    url: "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
    size: 35_099_487,
    sha256: "d0080766b6b74aeb81e1949ef0d9a20e6c17f7b160fb295a69b936ccb17660be",
};
#[cfg(not(target_os = "macos"))]
const WHEELS: [Pinned; 2] = [
    Pinned {
        file: "lz4-4.4.5-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
        url: "https://files.pythonhosted.org/packages/d2/fb/ba9256c48266a09012ed1d9b0253b9aa4fe9cdff094f8febf5b26a4aa2a2/lz4-4.4.5-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
        size: 1_368_257,
        sha256: "451039b609b9a88a934800b5fc6ee401c89ad9c175abf2f4d9f8b2e4ef1afc64",
    },
    Pinned {
        file: "capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
        url: "https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
        size: 1_485_316,
        sha256: "273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e",
    },
];
#[cfg(not(target_os = "macos"))]
const ZIG: Pinned = Pinned {
    file: "zig-x86_64-linux-0.17.0.tar.xz",
    url: "https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz",
    size: 57_332_648,
    sha256: "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026",
};

// macOS (Apple silicon): the same versions, built for aarch64-apple-darwin: python-build-standalone 20261013 (its SHA256SUMS), the
// PyPI wheels of the same lz4 and capstone releases for macOS 11+ arm64, Zig 0.17.0 aarch64-macos (ziglang.org download index).
#[cfg(target_os = "macos")]
const KIT: &str = "macOS";
#[cfg(target_os = "macos")]
const PYTHON: Pinned = Pinned {
    file: "cpython-3.13.16+20261013-aarch64-apple-darwin-install_only_stripped.tar.gz",
    url: "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-aarch64-apple-darwin-install_only_stripped.tar.gz",
    size: 25_268_458,
    sha256: "16a5332fb0dcb77f163fc8f8cbccd95be1644cc53e68e5d2a79f50afd6680a0e",
};
#[cfg(target_os = "macos")]
const WHEELS: [Pinned; 2] = [
    Pinned {
        file: "lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl",
        url: "https://files.pythonhosted.org/packages/8f/3f/ea3334e59de30871d773963997ecdba96c4584c5f8007fd83cfc8f1ee935/lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl",
        size: 207_163,
        sha256: "b424df1076e40d4e884cfcc4c77d815368b7fb9ebcd7e634f937725cd9a8a72a",
    },
    Pinned {
        file: "capstone-5.0.9-py3-none-macosx_11_0_arm64.whl",
        url: "https://files.pythonhosted.org/packages/42/9a/4eecfbc94961cda70301fd13de69042be1943bee456e37bc1369840601b1/capstone-5.0.9-py3-none-macosx_11_0_arm64.whl",
        size: 1_202_361,
        sha256: "fc269c5082e17d7f9f265cd6980e2fdc6ab572df913951d76db0357e06b8001d",
    },
];
#[cfg(target_os = "macos")]
const ZIG: Pinned = Pinned {
    file: "zig-aarch64-macos-0.17.0.tar.xz",
    url: "https://ziglang.org/download/0.17.0/zig-aarch64-macos-0.17.0.tar.xz",
    size: 53_985_220,
    sha256: "b607e9b9234790a008116ae5bdb71c6243b84b9fb42a53a9e70fde41c06c536a",
};

/// The native Python can run the release's scripts.
fn python_ready(py: &Path) -> bool {
    let mut c = command(py);
    c.args(["-I", "-c", "import lz4.block, capstone"]).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    c.status().map(|s| s.success()).unwrap_or(false)
}

/// Linux and macOS: fetches the native build tools into `native_tools` (tools/linux, tools/macos: python/ with lz4 and capstone,
/// zig/) unless they are there. `report(done, total, text)` gets the download progress. Ok(false) = cancelled. Windows:
/// nothing to do.
pub fn prepare_tools(tools_dir: &Path, cancel: &AtomicBool, report: &mut dyn FnMut(u64, u64, String)) -> Result<bool, String> {
    if cfg!(windows) {
        return Ok(true);
    }
    let kit = native_tools(tools_dir);
    let (py, zig) = (python(tools_dir), zig(tools_dir));
    let py_ok = python_ready(&py);
    if py_ok && zig.is_file() {
        return Ok(true);
    }
    let dl = kit.join("downloads");
    std::fs::create_dir_all(&dl).map_err(|e| format!("cannot create {}: {}", dl.display(), e))?;
    let mut need: Vec<&Pinned> = Vec::new();
    if !py_ok {
        need.push(&PYTHON);
        need.extend(WHEELS.iter());
    }
    if !zig.is_file() {
        need.push(&ZIG);
    }
    let total: u64 = need.iter().map(|p| p.size).sum();
    let mut done = 0u64;
    for p in &need {
        let mb = |b: u64| b as f64 / (1u64 << 20) as f64;
        let ok = fetch(p, &dl, cancel, &mut |n| {
            report(done + n, total, format!("Downloading the {} build tools: {:.0} of {:.0} MB", KIT, mb(done + n), mb(total)))
        })?;
        if !ok {
            return Ok(false);
        }
        done += p.size;
    }
    if !py_ok {
        report(total, total, format!("Installing the {} build tools: Python...", KIT));
        unpack(&dl.join(PYTHON.file), &kit, "python")?;
        let mut cmd = command(&py);
        cmd.args(["-I", "-m", "pip", "install", "-q", "--no-index", "--no-deps", "--no-compile", "--disable-pip-version-check", "--no-warn-script-location"]);
        cmd.args(WHEELS.iter().map(|w| dl.join(w.file)));
        let out = cmd.output().map_err(|e| format!("could not start {}: {}", py.display(), e))?;
        if !out.status.success() || !python_ready(&py) {
            return Err(format!("Installing lz4 and capstone into {} failed: {}", py.display(), String::from_utf8_lossy(&out.stderr).trim()));
        }
    }
    if !zig.is_file() {
        report(total, total, format!("Installing the {} build tools: Zig...", KIT));
        unpack(&dl.join(ZIG.file), &kit, "zig")?;
    }
    let _ = std::fs::remove_dir_all(&dl);
    Ok(true)
}

/// Downloads `p` into `dir` unless it is there. Ok(false) = cancelled. A file that does not match its pin is never kept.
fn fetch(p: &Pinned, dir: &Path, cancel: &AtomicBool, progress: &mut dyn FnMut(u64)) -> Result<bool, String> {
    use sha2::{Digest, Sha256};
    use std::io::{Read, Write};
    let dst = dir.join(p.file);
    let hex = |h: &[u8]| h.iter().map(|b| format!("{:02x}", b)).collect::<String>();
    if std::fs::read(&dst).map(|b| b.len() as u64 == p.size && hex(&Sha256::digest(&b)) == p.sha256).unwrap_or(false) {
        progress(p.size);
        return Ok(true);
    }
    let part = dir.join(format!("{}.part", p.file));
    let agent = ureq::AgentBuilder::new()
        .timeout_connect(std::time::Duration::from_secs(20))
        .timeout_read(std::time::Duration::from_secs(60))
        .user_agent(&format!("PokoMac/{}", env!("CARGO_PKG_VERSION")))
        .build();
    let resp = agent.get(p.url).call().map_err(|e| format!("Could not download {}: {}", p.url, e))?;
    let mut body = resp.into_reader().take(p.size + 1);
    let mut f = std::fs::File::create(&part).map_err(|e| format!("cannot write {}: {}", part.display(), e))?;
    let (mut h, mut n, mut buf) = (Sha256::new(), 0u64, vec![0u8; 1 << 16]);
    loop {
        if cancel.load(std::sync::atomic::Ordering::SeqCst) {
            drop(f);
            let _ = std::fs::remove_file(&part);
            return Ok(false);
        }
        let k = body.read(&mut buf).map_err(|e| format!("Downloading {} failed: {}", p.url, e))?;
        if k == 0 {
            break;
        }
        h.update(&buf[..k]);
        f.write_all(&buf[..k]).map_err(|e| format!("cannot write {}: {}", part.display(), e))?;
        n += k as u64;
        progress(n);
    }
    drop(f);
    let got = hex(&h.finalize());
    if n != p.size || got != p.sha256 {
        let _ = std::fs::remove_file(&part);
        return Err(format!("{} does not match its pinned size and SHA-256 ({} bytes, {}): not used. Try Set up again.", p.file, n, got));
    }
    std::fs::rename(&part, &dst).map_err(|e| format!("cannot move {}: {}", dst.display(), e))?;
    Ok(true)
}

/// Unpacks a tarball with one top-level folder (python/, zig-x86_64-linux-0.17.0/, zig-aarch64-macos-0.17.0/) as `parent/name`, replacing an older one.
fn unpack(archive: &Path, parent: &Path, name: &str) -> Result<(), String> {
    let tmp = parent.join(format!(".unpack-{}", name));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| format!("cannot create {}: {}", tmp.display(), e))?;
    let out = command("tar").arg("-xf").arg(archive).arg("-C").arg(&tmp).output().map_err(|e| format!("could not start tar: {}", e))?;
    if !out.status.success() {
        return Err(format!("Unpacking {} failed: {}", archive.display(), String::from_utf8_lossy(&out.stderr).trim()));
    }
    let top: Vec<PathBuf> = std::fs::read_dir(&tmp).map_err(|e| e.to_string())?.flatten().map(|e| e.path()).collect();
    let [only] = top.as_slice() else {
        return Err(format!("{} does not hold one folder", archive.display()));
    };
    let dst = parent.join(name);
    let _ = std::fs::remove_dir_all(&dst);
    std::fs::rename(only, &dst).map_err(|e| format!("cannot move {}: {}", dst.display(), e))?;
    let _ = std::fs::remove_dir_all(&tmp);
    Ok(())
}

/// pkp.exe loads pkp_rt.dll and the ASTC decoders from its own folder. Set up builds pkp.exe into the game folder, which can
/// be another folder than the release's: without these files Windows (Proton) ends it at once with STATUS_DLL_NOT_FOUND,
/// shown as exit code 53 (#221). Copies what is missing or different from the launcher's folder; nothing when they are one.
#[cfg(not(target_os = "macos"))]
pub fn sync_runtime_files(src: &Path, game_dir: &Path) -> Result<(), String> {
    let canon = |d: &Path| std::fs::canonicalize(d).unwrap_or_else(|_| d.to_path_buf());
    if canon(src) == canon(game_dir) {
        return Ok(());
    }
    let wanted = |n: &str| n == "pkp_rt.dll" || (n.starts_with("astcdec-") && n.ends_with(".dll"));
    let Ok(rd) = std::fs::read_dir(src) else { return Ok(()) };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if !wanted(&name) {
            continue;
        }
        let dst = game_dir.join(&name);
        let same = match (std::fs::metadata(e.path()), std::fs::metadata(&dst)) {
            (Ok(a), Ok(b)) => a.len() == b.len() && b.modified().ok() >= a.modified().ok(),
            _ => false,
        };
        if !same {
            std::fs::copy(e.path(), &dst).map_err(|err| format!("cannot copy {} into {}: {}", name, game_dir.display(), err))?;
        }
    }
    Ok(())
}

/// macOS: the game folder is never the app (the bundle stays as it was signed), so everything in the app's `runtime/` (`src`,
/// `runtime_source`) is copied into it at the same relative path before the game runs: `libpkp_rt.dylib` (pkp finds it next to itself,
/// @executable_path), the Vulkan loader `libvulkan.1.dylib`, the drivers `libvulkan_kosmickrisp.dylib` and `libMoltenVK.dylib` with
/// their manifests in `vulkan/icd.d/` (library_path relative to the json, runtime/platform/mac/gfx/vulkan_mac.cpp), and the fonts. A
/// file is copied when it is missing or its bytes differ, so an updated app refreshes the game folder on the next Play. Each copy is
/// written beside its target and renamed over it: a new file, never one rewritten in place, so a game still running keeps the
/// mapping it loaded and macOS never meets a signed library whose pages changed under its cached signature.
#[cfg(target_os = "macos")]
pub fn sync_runtime_files(src: &Path, game_dir: &Path) -> Result<(), String> {
    let canon = |d: &Path| std::fs::canonicalize(d).unwrap_or_else(|_| d.to_path_buf());
    if !src.is_dir() || canon(src) == canon(game_dir) {
        return Ok(());
    }
    let mut stack = vec![PathBuf::new()];
    while let Some(rel) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(src.join(&rel)) else { continue };
        for e in rd.flatten() {
            let r = rel.join(e.file_name());
            let from = src.join(&r);
            if from.is_dir() {
                stack.push(r);
                continue;
            }
            let dst = game_dir.join(&r);
            let same = match (std::fs::metadata(&from), std::fs::metadata(&dst)) {
                (Ok(a), Ok(b)) if a.len() == b.len() => std::fs::read(&from).ok().is_some_and(|x| std::fs::read(&dst).ok().is_some_and(|y| x == y)),
                _ => false,
            };
            if same {
                continue;
            }
            let failed = |err: std::io::Error| format!("cannot copy {} into {}: {}", r.display(), game_dir.display(), err);
            if let Some(dir) = dst.parent() {
                std::fs::create_dir_all(dir).map_err(failed)?;
            }
            let tmp = dst.with_file_name(format!(".{}.new", r.file_name().unwrap_or_default().to_string_lossy()));
            let copied = std::fs::copy(&from, &tmp).and_then(|_| std::fs::rename(&tmp, &dst));
            if let Err(err) = copied {
                let _ = std::fs::remove_file(&tmp);
                return Err(failed(err));
            }
        }
    }
    Ok(())
}

#[cfg(all(test, target_os = "macos"))]
mod mac_tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-host-test-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn the_app_bundle_keeps_its_tools_read_only_and_a_plain_folder_keeps_them_beside() {
        let d = scratch("bundle");
        let app = d.join("PokoMac.app");
        std::fs::create_dir_all(app.join("Contents/Resources/tools/build")).unwrap();
        std::fs::write(app.join("Contents/Info.plist"), "<plist/>").unwrap();
        let tools = app.join("Contents/Resources/tools");
        assert_eq!(bundle_of(&tools), Some(app.clone()));
        assert_eq!(bundle_of(&app.join("Contents/MacOS")), Some(app.clone()));
        assert_eq!(native_tools(&tools), support_dir().join("tools").join("macos"), "never inside the signed bundle");
        assert_eq!(python(&tools), support_dir().join("tools/macos/python/bin/python3"));
        assert_eq!(zig(&tools), support_dir().join("tools/macos/zig/zig"));
        assert_eq!(runtime_source(&tools), Some(app.join("Contents/Resources/runtime")));
        let plain = d.join("release").join("tools");
        assert_eq!(bundle_of(&plain), None);
        assert_eq!(native_tools(&plain), plain.join("macos"));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn the_runtime_folder_reaches_the_game_folder_and_a_changed_file_is_replaced_not_rewritten() {
        use std::os::unix::fs::MetadataExt;
        let d = scratch("sync");
        let (src, game) = (d.join("runtime"), d.join("game"));
        std::fs::create_dir_all(src.join("vulkan/icd.d")).unwrap();
        std::fs::create_dir_all(&game).unwrap();
        std::fs::write(src.join("libpkp_rt.dylib"), b"rt v1").unwrap();
        std::fs::write(src.join("vulkan/icd.d/libkosmickrisp_icd.json"), b"{}").unwrap();
        sync_runtime_files(&src, &game).unwrap();
        assert_eq!(std::fs::read(game.join("libpkp_rt.dylib")).unwrap(), b"rt v1");
        assert_eq!(std::fs::read(game.join("vulkan/icd.d/libkosmickrisp_icd.json")).unwrap(), b"{}");
        let ino = std::fs::metadata(game.join("libpkp_rt.dylib")).unwrap().ino();
        sync_runtime_files(&src, &game).unwrap();
        assert_eq!(std::fs::metadata(game.join("libpkp_rt.dylib")).unwrap().ino(), ino, "an identical file is left alone");
        std::fs::write(src.join("libpkp_rt.dylib"), b"rt v2").unwrap(); // same size, other bytes: an update
        sync_runtime_files(&src, &game).unwrap();
        assert_eq!(std::fs::read(game.join("libpkp_rt.dylib")).unwrap(), b"rt v2");
        assert_ne!(std::fs::metadata(game.join("libpkp_rt.dylib")).unwrap().ino(), ino, "a new file renamed over the old one");
        assert!(!game.join(".libpkp_rt.dylib.new").exists());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn this_process_is_found_by_its_name() {
        let mut buf = [0u8; 256];
        // SAFETY: proc_name writes at most buf.len() bytes
        let n = unsafe { libc::proc_name(libc::getpid(), buf.as_mut_ptr().cast(), buf.len() as u32) };
        assert!(n > 0);
        let me = String::from_utf8_lossy(&buf[..n as usize]).to_string();
        assert!(process_running(&me), "{me}");
        assert!(!process_running("no-such-process-pkp-test"));
    }
}
