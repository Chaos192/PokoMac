// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
//! Report a problem: what a bug report needs, in one click. The game's logs and reports, the setup log and the settings go into a zip on
//! the Desktop (`PokoMac-report-<date>-<time>.zip`; Downloads, then the game folder, when the Desktop cannot be written), Finder shows it,
//! and a new GitHub issue opens with the system facts (`system.txt`) already in it, so the player only has to attach the zip.
//!
//! What identifies the player is taken out before anything is written: the [account] values, the Link Play ids and room code, passwords
//! and tokens in pkp.ini and controls.ini, the same values wherever they appear in the logs, and the home folder in every path (`~`).
//! Saves, game files (exefs, romfs, .nca, .nsp), the translated shaders and the pipeline cache are never read: the zip takes a fixed list
//! of names (report_paths), nothing found by listing a folder.

use crate::ini::Ini;
use crate::settings::{CONTROLS_INI, PKP_INI};
use serde::Serialize;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

pub const ISSUES: &str = "https://github.com/Chaos192/PokoMac/issues/new";
/// Browsers and GitHub refuse very long links: the issue body is cut to fit.
const URL_MAX: usize = 6000;
/// A long session can leave a big log: only its end is kept, where a crash is.
const MAX_FILE: usize = 8 << 20;
const REMOVED: &str = "(removed)";

/// Where the zip went and what it holds.
#[derive(Serialize, Debug)]
pub struct Made {
    pub path: String,
    pub name: String,
    /// "on your Desktop", "in your Downloads folder" or "in the game folder"
    pub place: String,
    pub files: Vec<String>,
    /// the GitHub issue page was opened
    pub issue: bool,
}

/// Makes the zip, shows it in Finder and opens the GitHub issue. `config_file`: the launcher's launcher.json. `last_run`: how the last
/// game run ended (play.rs Status message), when it has ended, for the issue's title.
pub fn make(game_dir: &Path, config_file: &Path, version: &str, last_run: Option<&str>) -> Result<Made, String> {
    let secrets = secrets_of(game_dir);
    let home = home_dir();
    let clean = |text: &str| scrub(text, &secrets, home.as_deref());
    let mut entries: Vec<(String, Vec<u8>)> = Vec::new();
    let log_text = read_tail(&game_dir.join("pkp.log")).unwrap_or_default();

    let system = system_text(version, game_dir, last_run, &log_text);
    entries.push(("system.txt".into(), clean(&system).into_bytes()));
    for (name, path) in report_paths(game_dir) {
        if let Some(text) = read_tail(&path) {
            entries.push((name, clean(&text).into_bytes()));
        }
    }
    for name in [PKP_INI, CONTROLS_INI] {
        if let Ok(bytes) = fs::read(game_dir.join(name)) {
            entries.push((name.to_string(), clean(&scrub_ini(&String::from_utf8_lossy(&bytes))).into_bytes()));
        }
    }
    if let Ok(text) = fs::read_to_string(config_file) {
        entries.push(("launcher.json".into(), clean(&text).into_bytes()));
    }

    let name = format!("PokoMac-report-{}.zip", chrono::Local::now().format("%Y%m%d-%H%M%S"));
    let mut tried = Vec::new();
    let mut made = None;
    for (dir, place) in destinations(game_dir, home.as_deref()) {
        let path = dir.join(&name);
        match write_zip(&path, &entries) {
            Ok(()) => {
                made = Some((path, place));
                break;
            }
            Err(e) => {
                let _ = fs::remove_file(&path);
                tried.push(format!("{}: {}", crate::dump::fwd(&dir), e));
            }
        }
    }
    let Some((path, place)) = made else {
        return Err(format!("The report could not be saved ({}).", tried.join("; ")));
    };
    if let Err(e) = reveal(&path) {
        eprintln!("report: {}", e);
    }
    let body = format!(
        "{}\nPlease attach the zip from your {}: {} (drag it into this box). It holds the game's logs and settings, with your account and Link Play ids taken out.\n\n**What happened?**\n\n",
        clean(&system),
        if place == Place::Desktop { "Desktop" } else if place == Place::Downloads { "Downloads folder" } else { "game folder" },
        name
    );
    let title = title_of(last_run.map(clean).as_deref());
    let issue = crate::host::open(std::ffi::OsStr::new(&issue_url(&title, &body))).is_ok();
    Ok(Made { path: crate::dump::fwd(&path), name, place: place.words().into(), files: entries.into_iter().map(|(n, _)| n).collect(), issue })
}

/// The logs and reports, under the names they have in the zip.
fn report_paths(game_dir: &Path) -> Vec<(String, PathBuf)> {
    let mut v: Vec<(String, PathBuf)> = crate::play::REPORT_FILES.iter().map(|n| (n.to_string(), game_dir.join(n))).collect();
    v.push(("shader_cache/prepare_pipelines.log".into(), crate::shaders::cache_dir(game_dir).join("prepare_pipelines.log")));
    v.push((crate::setuplog::FILE.into(), crate::setuplog::path_of(game_dir)));
    v
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Place {
    Desktop,
    Downloads,
    GameFolder,
}

impl Place {
    fn words(self) -> &'static str {
        match self {
            Place::Desktop => "on your Desktop",
            Place::Downloads => "in your Downloads folder",
            Place::GameFolder => "in the game folder",
        }
    }
}

/// The Desktop, then Downloads (macOS may refuse either until the player allows it), then the game folder, which the launcher can
/// always write.
fn destinations(game_dir: &Path, home: Option<&Path>) -> Vec<(PathBuf, Place)> {
    let mut v = Vec::new();
    if let Some(h) = home {
        v.push((h.join("Desktop"), Place::Desktop));
        v.push((h.join("Downloads"), Place::Downloads));
    }
    v.retain(|(d, _)| d.is_dir());
    v.push((game_dir.to_path_buf(), Place::GameFolder));
    v
}

fn home_dir() -> Option<PathBuf> {
    ["HOME", "USERPROFILE"].iter().filter_map(std::env::var_os).map(PathBuf::from).find(|p| p.is_absolute())
}

fn write_zip(path: &Path, entries: &[(String, Vec<u8>)]) -> Result<(), String> {
    use chrono::{Datelike, Timelike};
    let file = fs::File::create(path).map_err(|e| e.to_string())?;
    let mut zip = zip::ZipWriter::new(file);
    let now = chrono::Local::now();
    let mut opts = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    if let Ok(t) = zip::DateTime::from_date_and_time(now.year() as u16, now.month() as u8, now.day() as u8, now.hour() as u8, now.minute() as u8, now.second() as u8) {
        opts = opts.last_modified_time(t);
    }
    for (name, bytes) in entries {
        zip.start_file(name, opts).map_err(|e| e.to_string())?;
        zip.write_all(bytes).map_err(|e| e.to_string())?;
    }
    zip.finish().map_err(|e| e.to_string())?.sync_all().map_err(|e| e.to_string())
}

/// Shows the zip in Finder (Explorer on Windows, the folder on Linux).
fn reveal(path: &Path) -> Result<(), String> {
    let mut cmd = if cfg!(target_os = "macos") {
        let mut c = crate::host::command("/usr/bin/open");
        c.arg("-R").arg(path);
        c
    } else if cfg!(windows) {
        let mut c = crate::host::command("explorer.exe");
        let mut arg = std::ffi::OsString::from("/select,");
        arg.push(path.as_os_str());
        c.arg(arg);
        c
    } else {
        return crate::host::open(path.parent().unwrap_or(path).as_os_str());
    };
    let mut child = cmd.spawn().map_err(|e| format!("Could not show {}: {}", path.display(), e))?;
    std::thread::spawn(move || child.wait());
    Ok(())
}

/// A text file, or its last MAX_FILE bytes with a line saying the start was left out. None when it does not exist.
fn read_tail(path: &Path) -> Option<String> {
    let bytes = fs::read(path).ok()?;
    if bytes.len() <= MAX_FILE {
        return Some(String::from_utf8_lossy(&bytes).into_owned());
    }
    let cut = bytes.len() - MAX_FILE;
    Some(format!("(the first {} bytes of this file were left out)\n{}", cut, String::from_utf8_lossy(&bytes[cut..])))
}

// ---- what identifies the player -------------------------------------------------------------------------------------------------------

/// Is `section.key` a value that identifies the player or opens something: the whole [account] section, the Link Play ids, room code and
/// password, and anything named like a token, password or secret.
fn sensitive(section: &str, key: &str) -> bool {
    let k = key.to_ascii_lowercase();
    section.eq_ignore_ascii_case("account")
        || (section.eq_ignore_ascii_case("net") && matches!(k.as_str(), "uid" | "room_code" | "password"))
        || ["token", "password", "secret", "passwd"].iter().any(|w| k.contains(w))
}

/// An ini file with those values replaced by `(removed)`; the key stays, so the report still shows that it was set.
fn scrub_ini(text: &str) -> String {
    let mut ini = Ini::parse(text);
    for (full, value) in ini.all() {
        let Some((section, key)) = full.split_once('.') else { continue };
        if sensitive(section, key) && !value.trim().is_empty() {
            ini.set(section, key, REMOVED);
        }
    }
    ini.to_text()
}

/// The values that identify this player, to take out of every file: the account and Link Play ids (also as the save folder's name),
/// the nickname, the room code, the Link Play id the runtime keeps in the save folder, and those of every profile. A profile's own name
/// is a label on this Mac only (and could be any word of a log line), so it stays.
fn secrets_of(game_dir: &Path) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    if let Ok(bytes) = fs::read(game_dir.join(PKP_INI)) {
        let ini = Ini::parse(&String::from_utf8_lossy(&bytes));
        for (full, value) in ini.all() {
            if let Some((s, k)) = full.split_once('.') {
                if sensitive(s, k) {
                    out.push(value);
                }
            }
        }
    }
    for p in crate::profiles::load(game_dir).profiles {
        out.extend([p.uid, p.net_uid, p.nickname]);
    }
    let (sd, key) = crate::savebackup::locate(game_dir);
    out.push(key);
    for f in [sd.join("coop-uid.txt"), game_dir.join("room-code.txt")] {
        if let Ok(t) = fs::read_to_string(f) {
            out.push(t);
        }
    }
    let mut extra: Vec<String> = out.iter().filter_map(|v| crate::saveimport::uid_key(v)).collect();
    out.append(&mut extra);
    // the runtime's own default nickname says nothing about the player, and a short word would cut into other words of the logs
    let mut out: Vec<String> = out.into_iter().map(|v| v.trim().trim_matches('"').to_string()).filter(|v| v.chars().count() >= 3 && v != "Player").collect();
    out.sort_by_key(|v| std::cmp::Reverse(v.len()));
    out.dedup();
    out
}

/// `text` with every secret (as a whole word, any case) replaced by `(removed)` and the home folder written as `~`.
fn scrub(text: &str, secrets: &[String], home: Option<&Path>) -> String {
    let mut t = text.to_string();
    if let Some(h) = home.map(|h| h.to_string_lossy().trim_end_matches(['/', '\\']).to_string()).filter(|h| h.len() > 1) {
        t = t.replace(&h, "~");
        if h.contains('\\') {
            t = t.replace(&h.replace('\\', "/"), "~");
        }
    }
    for s in secrets {
        t = replace_word(&t, s, REMOVED);
    }
    t
}

/// Replaces `word` where it stands alone (no letter or digit right before or after it), ignoring ASCII case.
fn replace_word(text: &str, word: &str, with: &str) -> String {
    if word.is_empty() {
        return text.to_string();
    }
    let (hay, needle) = (text.to_ascii_lowercase(), word.to_ascii_lowercase());
    let is_word = |c: Option<char>| c.is_some_and(|c| c.is_alphanumeric());
    let mut out = String::with_capacity(text.len());
    let mut last = 0;
    let mut from = 0;
    while let Some(i) = hay[from..].find(&needle).map(|i| i + from) {
        let end = i + needle.len();
        if !is_word(text[..i].chars().next_back()) && !is_word(text[end..].chars().next()) {
            out.push_str(&text[last..i]);
            out.push_str(with);
            last = end;
        }
        from = end;
    }
    out.push_str(&text[last..]);
    out
}

// ---- system.txt and the issue ---------------------------------------------------------------------------------------------------------

/// The output of a system tool, trimmed; empty when it fails.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn run(program: &str, args: &[&str]) -> String {
    let out = crate::host::command(program).args(args).stdin(std::process::Stdio::null()).stderr(std::process::Stdio::null()).output();
    out.map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string()).unwrap_or_default()
}

/// The facts a bug needs first: the system, the Mac, the launcher, and the graphics driver lines of the last pkp.log.
fn system_text(version: &str, game_dir: &Path, last_run: Option<&str>, log: &str) -> String {
    let mut s = String::from("PokoMac problem report\n\n");
    s.push_str(&format!("Launcher: {} ({} {})\n", version, std::env::consts::OS, std::env::consts::ARCH));
    #[cfg(target_os = "macos")]
    {
        let sw: Vec<String> = run("/usr/bin/sw_vers", &[]).lines().filter_map(|l| l.split_once(':')).map(|(_, v)| v.trim().to_string()).collect();
        s.push_str(&format!("macOS: {}\n", if sw.is_empty() { "unknown".into() } else { sw.join(" ") }));
        s.push_str(&format!("Mac model: {}\n", run("/usr/sbin/sysctl", &["-n", "hw.model"])));
        s.push_str(&format!("Chip: {}\n", run("/usr/sbin/sysctl", &["-n", "machdep.cpu.brand_string"])));
        let mem: u64 = run("/usr/sbin/sysctl", &["-n", "hw.memsize"]).parse().unwrap_or(0);
        s.push_str(&format!("Memory: {} GB\n", mem >> 30));
    }
    s.push_str(&format!("Game built: {}\n", if game_dir.join(crate::host::GAME_EXE).is_file() { "yes" } else { "no" }));
    if let Some(r) = last_run.filter(|r| !r.trim().is_empty()) {
        s.push_str(&format!("Last run: {}\n", r.trim()));
    }
    let lines = driver_lines(log);
    if !lines.is_empty() {
        s.push_str("\nFrom pkp.log:\n");
        for l in lines {
            s.push_str(&format!("  {}\n", l));
        }
    }
    s
}

/// The Vulkan driver and Metal lines of a pkp.log (runtime platform/mac/gfx/vulkan_mac.cpp, metal_mac.mm), and the device it chose, the
/// first of each.
fn driver_lines(log: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for want in ["Vulkan driver", "Metal: ", "device ready: "] {
        if let Some(l) = log.lines().find(|l| l.contains(want)) {
            out.push(l.trim().chars().take(300).collect());
        }
    }
    out
}

/// The issue's title: what the last run ended with, when there is a message to give it.
fn title_of(last_run: Option<&str>) -> String {
    let base = "Problem report";
    match last_run.map(str::trim).filter(|r| !r.is_empty()) {
        Some(r) => format!("{}: {}", base, r.chars().take(90).collect::<String>()),
        None => base.to_string(),
    }
}

/// Percent-encodes everything but the unreserved characters (RFC 3986).
fn encode(s: &str, out: &mut String, limit: usize) -> bool {
    for c in s.chars() {
        let mut buf = [0u8; 4];
        let piece: String = if c.is_ascii_alphanumeric() || "-_.~".contains(c) {
            c.to_string()
        } else {
            c.encode_utf8(&mut buf).bytes().map(|b| format!("%{:02X}", b)).collect()
        };
        if out.len() + piece.len() > limit {
            return false;
        }
        out.push_str(&piece);
    }
    true
}

/// The new-issue link with the title and body filled in, at most URL_MAX characters: the body is cut (whole characters) when longer.
pub fn issue_url(title: &str, body: &str) -> String {
    let mut url = format!("{}?title=", ISSUES);
    encode(title, &mut url, URL_MAX);
    url.push_str("&body=");
    encode(body, &mut url, URL_MAX);
    url
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_link_is_encoded_and_cut_to_fit() {
        let u = issue_url("Problem report: pkp was ended by signal 11", "Mac model: Mac17,6\nChip: Apple M5 Max & more?");
        assert!(u.starts_with("https://github.com/Chaos192/PokoMac/issues/new?title=Problem%20report%3A%20pkp"));
        assert!(u.contains("&body=Mac%20model%3A%20Mac17%2C6%0AChip%3A%20Apple%20M5%20Max%20%26%20more%3F"));
        assert_eq!(u.matches('&').count(), 1, "only the separator is a bare &");
        let long = issue_url("t", &"é".repeat(5000));
        assert!(long.len() <= URL_MAX && long.ends_with("%C3%A9"), "cut on a whole character");
    }

    #[test]
    fn ids_and_codes_leave_the_ini() {
        let text = "[account]\nuid = 0123456789abcdef0123456789abcdef\nnickname = Ash ; me\n[net]\nmode = auto\nuid = fedcba9876543210fedcba9876543210\nroom_code = ABCD-EFGH-JKMN-PQRS\n[video]\nwidth = 1920\napi_token = abc\n";
        let out = scrub_ini(text);
        let ini = Ini::parse(&out);
        for (s, k) in [("account", "uid"), ("account", "nickname"), ("net", "uid"), ("net", "room_code"), ("video", "api_token")] {
            assert_eq!(ini.get(s, k).as_deref(), Some(REMOVED), "{s}.{k}");
        }
        assert_eq!(ini.get("net", "mode").as_deref(), Some("auto"));
        assert_eq!(ini.get("video", "width").as_deref(), Some("1920"));
        assert!(out.contains("; me"), "comments stay");
    }

    #[test]
    fn secrets_go_as_whole_words_and_the_home_folder_becomes_a_tilde() {
        let secrets = vec!["Misty".to_string(), "0123456789abcdef0123456789abcdef".to_string()];
        let out = scrub("player Misty joined; MISTY again; Mistyland stays; uid 0123456789ABCDEF0123456789ABCDEF\nsave in /Users/player/Library/x", &secrets, Some(Path::new("/Users/player")));
        assert_eq!(out, "player (removed) joined; (removed) again; Mistyland stays; uid (removed)\nsave in ~/Library/x");
    }

    #[test]
    fn only_the_first_driver_lines_are_taken() {
        let log = "a\n[I gfx] Vulkan driver: kosmickrisp ([gfx] vulkan_driver = auto)\n[I gfx] Metal: Apple M5 Max compiles up to 8 shaders at once\n[I vk] device ready: Apple M5 Max, queue family 0\n[I gfx] Vulkan driver: again\n";
        let l = driver_lines(log);
        assert_eq!(l.len(), 3);
        assert!(l[0].contains("kosmickrisp") && l[1].contains("Metal:") && l[2].contains("device ready"));
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn system_txt_names_the_mac() {
        let s = system_text("9.9.9", Path::new("/nonexistent"), Some("pkp exited normally."), "");
        for want in ["Launcher: 9.9.9 (macos aarch64)", "macOS: macOS ", "Mac model: Mac", "Chip: ", "Memory: ", "Last run: pkp exited normally."] {
            assert!(s.contains(want), "{want:?} in\n{s}");
        }
        assert!(!s.contains("Memory: 0 GB") && !s.contains("Chip: \n"), "{s}");
    }

    #[test]
    fn a_zip_is_written_with_every_entry() {
        let dir = std::env::temp_dir().join(format!("pkp-report-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("r.zip");
        write_zip(&path, &[("system.txt".into(), b"hello".to_vec()), ("pkp.ini".into(), b"[video]\n".to_vec())]).unwrap();
        let bytes = fs::read(&path).unwrap();
        assert!(bytes.starts_with(b"PK\x03\x04"));
        assert!(bytes.windows(10).any(|w| w == b"system.txt"));
        let _ = fs::remove_dir_all(&dir);
    }
}
