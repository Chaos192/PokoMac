// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//! Timed-event selection lives in pkp.ini; its clock offset is calculated fresh for every game launch.
//! The ordinary Game clock is never overwritten: the selected event is applied only to pkp.exe's environment.

use chrono::{DateTime, Local, TimeZone};
use std::fs;
use std::path::Path;
use std::process::Command;

use crate::ini::Ini;
use crate::settings::PKP_INI;

const CLOCK_LIMIT_MIN: i64 = 10 * 366 * 24 * 60;

struct Event {
    id: &'static str,
    year: i32,
    month: u32,
    day: u32,
}

const EVENTS: &[Event] = &[
    Event { id: "copycat", year: 2026, month: 4, day: 2 },
    Event { id: "hoppip", year: 2026, month: 3, day: 12 },
    Event { id: "bulbasaur", year: 2026, month: 4, day: 21 },
    Event { id: "sableye", year: 2026, month: 5, day: 1 },
];

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClockOverride {
    minutes: i64,
}

impl ClockOverride {
    /// Environment keys understood by runtime/src/services/time.cpp. No ini value is changed.
    pub fn apply_to(&self, cmd: &mut Command) {
        cmd.env("NXRT_TIME_CLOCK_OFFSET", self.minutes.to_string());
        cmd.env("NXRT_TIME_CLOCK_SCOPE", "all");
    }
}

/// The selected event's clock for this launch. Missing and off preserve the ordinary Game clock.
pub fn override_for(dir: &Path) -> Result<Option<ClockOverride>, String> {
    override_at(dir, &Local::now())
}

fn override_at(dir: &Path, now: &DateTime<Local>) -> Result<Option<ClockOverride>, String> {
    let text = fs::read_to_string(dir.join(PKP_INI)).unwrap_or_default();
    let ini = Ini::parse(&text);
    let id = ini.get("time", "event_preset").unwrap_or_default().trim().to_ascii_lowercase();
    if id.is_empty() || id == "off" {
        return Ok(None);
    }
    let event = EVENTS.iter().find(|e| e.id == id).ok_or_else(|| {
        format!("Unknown timed event '{}' in pkp.ini. Choose a valid event or turn all event toggles off.", id)
    })?;
    let target = Local.with_ymd_and_hms(event.year, event.month, event.day, 9, 0, 0)
        .single()
        .ok_or_else(|| format!("The local start time for timed event '{}' is not available in this time zone.", id))?;
    let seconds = target.timestamp() - now.timestamp();
    let minutes = (seconds as f64 / 60.0).round() as i64;
    Ok(Some(ClockOverride { minutes: minutes.clamp(-CLOCK_LIMIT_MIN, CLOCK_LIMIT_MIN) }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsStr;
    use std::sync::atomic::{AtomicUsize, Ordering};

    fn tmp(text: Option<&str>) -> std::path::PathBuf {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let dir = std::env::temp_dir().join(format!("pkp-events-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::SeqCst)));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        if let Some(text) = text {
            fs::write(dir.join(PKP_INI), text).unwrap();
        }
        dir
    }

    fn now(year: i32, month: u32, day: u32, hour: u32, minute: u32) -> DateTime<Local> {
        Local.with_ymd_and_hms(year, month, day, hour, minute, 30).single().unwrap()
    }

    #[test]
    fn every_event_uses_its_local_start_date() {
        let now = now(2026, 11, 8, 12, 0);
        for (id, year, month, day) in [("copycat", 2026, 4, 2), ("hoppip", 2026, 3, 12), ("bulbasaur", 2026, 4, 21), ("sableye", 2026, 5, 1)] {
            let event = EVENTS.iter().find(|event| event.id == id).unwrap();
            assert_eq!((event.year, event.month, event.day), (year, month, day));
            let dir = tmp(Some(&format!("[time]\nevent_preset = {}\n", id)));
            let got = override_at(&dir, &now).unwrap().unwrap();
            let target = Local.with_ymd_and_hms(year, month, day, 9, 0, 0).single().unwrap();
            let expected = ((target.timestamp() - now.timestamp()) as f64 / 60.0).round() as i64;
            assert_eq!(got.minutes, expected, "{}", id);
            let _ = fs::remove_dir_all(dir);
        }
    }

    #[test]
    fn a_later_launch_recalculates_the_offset() {
        let dir = tmp(Some("[time]\nevent_preset = copycat\n"));
        let first = override_at(&dir, &now(2026, 11, 8, 12, 0)).unwrap().unwrap();
        let second = override_at(&dir, &now(2026, 11, 9, 12, 0)).unwrap().unwrap();
        assert_eq!(first.minutes - second.minutes, 1440);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn missing_empty_and_off_keep_the_normal_clock() {
        for text in [None, Some(""), Some("[time]\nevent_preset =\n"), Some("[time]\nevent_preset = off\n")] {
            let dir = tmp(text);
            assert_eq!(override_at(&dir, &now(2026, 11, 8, 12, 0)).unwrap(), None);
            let _ = fs::remove_dir_all(dir);
        }
    }

    #[test]
    fn an_unknown_event_stops_launch() {
        let dir = tmp(Some("[time]\nevent_preset = something_else\n"));
        let error = override_at(&dir, &now(2026, 11, 8, 12, 0)).unwrap_err();
        assert!(error.contains("something_else"));
        assert!(error.contains("turn all event toggles off"));
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn selecting_an_event_does_not_rewrite_the_ini() {
        let text = "[time]\nclock_offset = 123\nclock_scope = user\nevent_preset = copycat\n";
        let dir = tmp(Some(text));
        override_at(&dir, &now(2026, 11, 8, 12, 0)).unwrap();
        assert_eq!(fs::read_to_string(dir.join(PKP_INI)).unwrap(), text);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn the_override_reaches_only_the_game_command() {
        let mut cmd = Command::new("pkp-test");
        ClockOverride { minutes: -12345 }.apply_to(&mut cmd);
        let get = |name: &str| {
            cmd.get_envs()
                .find(|(key, _)| *key == OsStr::new(name))
                .and_then(|(_, value)| value)
                .map(|value| value.to_string_lossy().into_owned())
        };
        assert_eq!(get("NXRT_TIME_CLOCK_OFFSET").as_deref(), Some("-12345"));
        assert_eq!(get("NXRT_TIME_CLOCK_SCOPE").as_deref(), Some("all"));
    }
}
