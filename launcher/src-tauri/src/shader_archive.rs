// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//! The shader archive: every translated shader in a few compressed files instead of ~99,000 loose `<hash>.spv` files.
//!
//! `shader_cache/spirv.archive/NNNNNN.pack` (beside `shader_cache/spirv`, like the runtime's preload pack) are segments in the
//! layout of the runtime's preload pack (runtime nvn_shaders.cpp, "the preload pack" and "the shader archive"): a 32-byte header,
//! one zstd frame per shader, then an index of 40-byte entries. The runtime reads them; this file writes them and reads them back
//! for the launcher's own checks.
//!
//! Rules both sides keep:
//!   - a higher segment number wins when a hash is in two segments;
//!   - a loose `<hash>.spv` in `shader_cache/spirv` wins over the archive (a shader translated again is written there and used at once);
//!   - `results.tsv` stays a loose file: it is what makes a shader count as translated.
//!
//! A segment is written as `.pack.tmp`, flushed, then renamed, and loose files are only deleted after that: a crash leaves either the
//! loose file or the archived copy, never neither.
//!
//! No crate-local imports, so the examples that build shaders.rs alone can include this file too.

use std::collections::HashMap;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

/// The archive folder, in the game folder.
pub const ARCHIVE_REL: &str = "shader_cache/spirv.archive";
/// Where loose files live (shaders.rs SPIRV_REL).
const SPIRV_REL: &str = "shader_cache/spirv";
/// zstd level. Decompression speed is the same at every level, so the game reads the archive as fast at 12 as at 6 (the preload pack:
/// ~3.8x). Higher levels only cost the launcher time when it writes.
pub const LEVEL: i32 = 12;
/// A segment is closed at this much SPIR-V (or SEGMENT_MAX shaders): big enough that the game opens a few dozen files, small enough that
/// a pause loses little work.
const SEGMENT_RAW: u64 = 512 << 20;
const SEGMENT_MAX: usize = 4000;
/// Before anything is measured, "Make smaller" promises what the preload pack measured at level 6 (~3.8x): never more than it gets.
pub const RATIO_GUESS: f64 = 3.8;

const MAGIC: &[u8; 8] = b"NXSPACK1";
const VERSION: u32 = 1;
const HEADER: u64 = 32;
const ENTRY: u64 = 40;
const MAX_SHADER: u32 = 64 << 20;

#[derive(Clone, Copy)]
struct Entry {
    seg: usize,
    offset: u64,
    csize: u32,
    usize: u32,
}

/// What is in the archive. `open` reads only the segments' indexes.
#[derive(Default)]
pub struct Archive {
    segs: Vec<PathBuf>,
    map: HashMap<String, Entry>,
    /// bytes on disk (all segments) and the SPIR-V they hold (the newest copy of each shader)
    pub disk_bytes: u64,
    pub raw_bytes: u64,
}

pub fn archive_dir(game_dir: &Path) -> PathBuf {
    game_dir.join(ARCHIVE_REL)
}

/// The segment files in order: (number, path).
fn segments(dir: &Path) -> Vec<(u32, PathBuf)> {
    let mut out: Vec<(u32, PathBuf)> = fs::read_dir(dir)
        .map(|rd| {
            rd.filter_map(Result::ok)
                .filter_map(|e| {
                    let name = e.file_name().to_string_lossy().to_string();
                    let num = name.strip_suffix(".pack").filter(|n| n.len() == 6 && n.bytes().all(|b| b.is_ascii_digit()))?;
                    Some((num.parse().ok()?, e.path()))
                })
                .collect()
        })
        .unwrap_or_default();
    out.sort();
    out
}

/// True when the game folder has an archive with at least one segment.
pub fn exists(game_dir: &Path) -> bool {
    !segments(&archive_dir(game_dir)).is_empty()
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

/// One index entry: (hash, offset, csize, usize).
type IndexEntry = (String, u64, u32, u32);

/// The index of one segment and where its data ends, or None if the file is not a segment.
fn read_index(path: &Path) -> Option<(Vec<IndexEntry>, u64)> {
    let mut f = fs::File::open(path).ok()?;
    let size = f.metadata().ok()?.len();
    let mut h = [0u8; HEADER as usize];
    f.read_exact(&mut h).ok()?;
    let (count, index) = (u32_at(&h, 12) as u64, u64_at(&h, 16));
    if &h[..8] != MAGIC || u32_at(&h, 8) != VERSION || count > 1_000_000 || index < HEADER || index + count * ENTRY != size {
        return None;
    }
    let mut idx = vec![0u8; (count * ENTRY) as usize];
    f.seek(SeekFrom::Start(index)).ok()?;
    f.read_exact(&mut idx).ok()?;
    let mut out = Vec::with_capacity(count as usize);
    for e in idx.chunks_exact(ENTRY as usize) {
        let (offset, csize, usize) = (u64_at(e, 24), u32_at(e, 32), u32_at(e, 36));
        if offset < HEADER || csize == 0 || offset + csize as u64 > index || usize == 0 || usize % 4 != 0 || usize > MAX_SHADER {
            continue;
        }
        out.push((String::from_utf8_lossy(&e[..16]).to_string(), offset, csize, usize));
    }
    Some((out, index))
}

impl Archive {
    /// Reads every segment's index. An unreadable segment is skipped, as the runtime does.
    pub fn open(game_dir: &Path) -> Archive {
        let mut a = Archive::default();
        for (_, path) in segments(&archive_dir(game_dir)) {
            let Some((entries, data_end)) = read_index(&path) else { continue };
            let seg = a.segs.len();
            a.segs.push(path);
            a.disk_bytes += data_end + entries.len() as u64 * ENTRY;
            for (hash, offset, csize, usize) in entries {
                a.map.insert(hash, Entry { seg, offset, csize, usize });
            }
        }
        a.raw_bytes = a.map.values().map(|e| e.usize as u64).sum();
        a
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub fn contains(&self, hash: &str) -> bool {
        self.map.contains_key(hash)
    }

    /// The shader's SPIR-V bytes.
    pub fn read(&self, hash: &str) -> Option<Vec<u8>> {
        let e = *self.map.get(hash)?;
        let mut f = fs::File::open(&self.segs[e.seg]).ok()?;
        f.seek(SeekFrom::Start(e.offset)).ok()?;
        let mut comp = vec![0u8; e.csize as usize];
        f.read_exact(&mut comp).ok()?;
        zstd::bulk::decompress(&comp, e.usize as usize).ok().filter(|b| b.len() == e.usize as usize)
    }
}

/// True when the shader has a translation: a loose `<hash>.spv` or an archived copy.
pub fn has_translation(spirv: &Path, archive: &Archive, hash: &str) -> bool {
    archive.contains(hash) || spirv.join(format!("{}.spv", hash)).is_file()
}

/// The shader's SPIR-V: the loose file wins, as in the runtime.
pub fn read_translation(spirv: &Path, archive: &Archive, hash: &str) -> Option<Vec<u8>> {
    fs::read(spirv.join(format!("{}.spv", hash))).ok().or_else(|| archive.read(hash))
}

/// Compresses `items` (hash, SPIR-V) on every core and writes them as the next segment. Returns the segment's size on disk.
fn write_segment(dir: &Path, items: &[(String, Vec<u8>)]) -> Result<u64, String> {
    fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2).clamp(1, 16);
    let per = items.len().div_ceil(threads).max(1);
    let frames: Vec<Result<Vec<u8>, String>> = std::thread::scope(|s| {
        let parts: Vec<_> = items.chunks(per).map(|part| s.spawn(move || part.iter().map(|(_, raw)| zstd::bulk::compress(raw, LEVEL).map_err(|e| e.to_string())).collect::<Vec<_>>())).collect();
        parts.into_iter().flat_map(|p| p.join().unwrap_or_else(|_| vec![Err("a compression thread failed".into())])).collect()
    });
    let next = segments(dir).last().map_or(1, |(n, _)| n + 1);
    let fin = dir.join(format!("{:06}.pack", next));
    let tmp = dir.join(format!("{:06}.pack.tmp", next));
    let werr = |e: std::io::Error| format!("cannot write {}: {}", tmp.display(), e);
    let mut f = std::io::BufWriter::with_capacity(8 << 20, fs::File::create(&tmp).map_err(werr)?);
    f.write_all(&[0u8; HEADER as usize]).map_err(werr)?;
    let mut off = HEADER;
    let mut index = Vec::with_capacity(items.len() * ENTRY as usize);
    for ((hash, raw), frame) in items.iter().zip(frames) {
        let frame = frame.map_err(|e| format!("cannot compress shader {}: {}", hash, e))?;
        f.write_all(&frame).map_err(werr)?;
        let mut name = [0u8; 16];
        name.copy_from_slice(&hash.as_bytes()[..16]);
        index.extend_from_slice(&name);
        index.extend_from_slice(&0u64.to_le_bytes()); // line_hash: not used by the archive
        index.extend_from_slice(&off.to_le_bytes());
        index.extend_from_slice(&(frame.len() as u32).to_le_bytes());
        index.extend_from_slice(&(raw.len() as u32).to_le_bytes());
        off += frame.len() as u64;
    }
    f.write_all(&index).map_err(werr)?;
    let mut head = Vec::with_capacity(HEADER as usize);
    head.extend_from_slice(MAGIC);
    head.extend_from_slice(&VERSION.to_le_bytes());
    head.extend_from_slice(&(items.len() as u32).to_le_bytes());
    head.extend_from_slice(&off.to_le_bytes());
    head.extend_from_slice(&0u64.to_le_bytes());
    let mut f = f.into_inner().map_err(|e| werr(e.into_error()))?;
    f.seek(SeekFrom::Start(0)).map_err(werr)?;
    f.write_all(&head).map_err(werr)?;
    // on the disk before the rename, and the rename before any loose file is deleted
    f.sync_all().map_err(werr)?;
    drop(f);
    fs::rename(&tmp, &fin).map_err(|e| format!("cannot rename {}: {}", tmp.display(), e))?;
    Ok(off + items.len() as u64 * ENTRY)
}

/// A loose file the archive takes: `<16 hex digits>.spv`.
fn loose_hash(name: &str) -> Option<&str> {
    let h = name.strip_suffix(".spv")?;
    (h.len() == 16 && h.bytes().all(|b| b.is_ascii_hexdigit())).then_some(h)
}

/// The loose shader files: (hash, size).
pub fn loose_files(game_dir: &Path) -> Vec<(String, u64)> {
    let dir = game_dir.join(SPIRV_REL);
    fs::read_dir(&dir)
        .map(|rd| {
            rd.filter_map(Result::ok)
                .filter_map(|e| {
                    let name = e.file_name().to_string_lossy().to_string();
                    let h = loose_hash(&name)?.to_string();
                    Some((h, e.metadata().ok()?.len()))
                })
                .collect()
        })
        .unwrap_or_default()
}

#[derive(Clone, Copy, Default, serde::Serialize)]
pub struct Moved {
    /// shaders moved into the archive, the SPIR-V they hold, what they take in the archive
    pub shaders: u64,
    pub raw_bytes: u64,
    pub disk_bytes: u64,
    pub seconds: f64,
}

/// Moves the loose files whose hash is in `only` (None: every loose file) into new segments, deleting each after its segment is on disk.
/// `progress(done, total)`. Stops between shaders when `cancel` is set (what is moved stays moved). A file that is not valid SPIR-V
/// (empty, not whole words) stays loose. Ok((moved, finished)).
pub fn compact(game_dir: &Path, only: Option<&[String]>, cancel: &AtomicBool, mut progress: impl FnMut(u64, u64)) -> Result<(Moved, bool), String> {
    let t0 = std::time::Instant::now();
    let spirv = game_dir.join(SPIRV_REL);
    let dir = archive_dir(game_dir);
    let mut todo = loose_files(game_dir);
    if let Some(only) = only {
        let want: std::collections::HashSet<&str> = only.iter().map(String::as_str).collect();
        todo.retain(|(h, _)| want.contains(h.as_str()));
    }
    // in name order: a pause and a continue walk the same list
    todo.sort();
    let total = todo.len() as u64;
    let mut moved = Moved::default();
    let mut batch: Vec<(String, Vec<u8>)> = Vec::new();
    let mut batch_raw = 0u64;
    let mut done = 0u64;
    progress(0, total);
    let flush = |batch: &mut Vec<(String, Vec<u8>)>, batch_raw: &mut u64, moved: &mut Moved| -> Result<(), String> {
        if batch.is_empty() {
            return Ok(());
        }
        moved.disk_bytes += write_segment(&dir, batch)?;
        for (h, raw) in batch.iter() {
            let _ = fs::remove_file(spirv.join(format!("{}.spv", h)));
            moved.raw_bytes += raw.len() as u64;
        }
        moved.shaders += batch.len() as u64;
        batch.clear();
        *batch_raw = 0;
        Ok(())
    };
    for (h, _) in &todo {
        if cancel.load(Ordering::SeqCst) {
            flush(&mut batch, &mut batch_raw, &mut moved)?;
            moved.seconds = t0.elapsed().as_secs_f64();
            return Ok((moved, false));
        }
        done += 1;
        let Ok(raw) = fs::read(spirv.join(format!("{}.spv", h))) else { continue };
        if raw.is_empty() || raw.len() % 4 != 0 || raw.len() as u64 > MAX_SHADER as u64 {
            continue;
        }
        batch_raw += raw.len() as u64;
        batch.push((h.clone(), raw));
        if batch_raw >= SEGMENT_RAW || batch.len() >= SEGMENT_MAX {
            flush(&mut batch, &mut batch_raw, &mut moved)?;
        }
        if done.is_multiple_of(250) || done == total {
            progress(done, total);
        }
    }
    flush(&mut batch, &mut batch_raw, &mut moved)?;
    progress(total, total);
    moved.seconds = t0.elapsed().as_secs_f64();
    Ok((moved, true))
}

/// The other way: writes every archived shader back as a loose file (one that already has a loose file is kept as it is) and deletes the
/// segments, one at a time. Ok(finished).
pub fn expand(game_dir: &Path, cancel: &AtomicBool, mut progress: impl FnMut(u64, u64)) -> Result<bool, String> {
    let spirv = game_dir.join(SPIRV_REL);
    fs::create_dir_all(&spirv).map_err(|e| format!("cannot create {}: {}", spirv.display(), e))?;
    let a = Archive::open(game_dir);
    let total = a.len() as u64;
    let mut done = 0u64;
    progress(0, total);
    for seg in 0..a.segs.len() {
        // the shaders whose newest copy is in this segment
        let mut here: Vec<&String> = a.map.iter().filter(|(_, e)| e.seg == seg).map(|(h, _)| h).collect();
        here.sort();
        for h in here {
            if cancel.load(Ordering::SeqCst) {
                return Ok(false);
            }
            done += 1;
            let dst = spirv.join(format!("{}.spv", h));
            if !dst.is_file() {
                let raw = a.read(h).ok_or_else(|| format!("shader {} in {} cannot be read", h, a.segs[seg].display()))?;
                let tmp = spirv.join(format!("{}.spv.tmp", h));
                fs::write(&tmp, &raw).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
                fs::rename(&tmp, &dst).map_err(|e| format!("cannot write {}: {}", dst.display(), e))?;
            }
            if done.is_multiple_of(250) {
                progress(done, total);
            }
        }
        // every shader whose newest copy is here, and every older copy here (a later segment has it), is loose now or in a later segment
        fs::remove_file(&a.segs[seg]).map_err(|e| format!("cannot delete {}: {}", a.segs[seg].display(), e))?;
    }
    let _ = fs::remove_dir(archive_dir(game_dir));
    progress(total, total);
    Ok(true)
}

/// Deletes a `.pack.tmp` a crash left behind.
pub fn clean(game_dir: &Path) {
    if let Ok(rd) = fs::read_dir(archive_dir(game_dir)) {
        for e in rd.filter_map(Result::ok) {
            if e.file_name().to_string_lossy().ends_with(".pack.tmp") {
                let _ = fs::remove_file(e.path());
            }
        }
    }
}

#[derive(Clone, Default, serde::Serialize)]
pub struct Storage {
    /// "none" (nothing translated yet), "loose", "compact", "mixed" (an unfinished move, or shaders translated since)
    pub mode: String,
    pub loose_files: u64,
    pub loose_bytes: u64,
    pub archive_shaders: u64,
    pub archive_bytes: u64,
    pub archive_raw_bytes: u64,
    /// what "Make smaller" frees, roughly: the loose bytes less what they will take compressed (the archive's own ratio once there is
    /// one, else RATIO_GUESS)
    pub free_estimate: u64,
}

/// How the shaders are stored now. Lists the cache folder (a second or two for 99,000 files on a hard drive).
pub fn storage(game_dir: &Path) -> Storage {
    let loose = loose_files(game_dir);
    let a = Archive::open(game_dir);
    let loose_bytes: u64 = loose.iter().map(|(_, n)| n).sum();
    let ratio = if a.disk_bytes > 0 && a.raw_bytes > 0 { (a.raw_bytes as f64 / a.disk_bytes as f64).max(1.0) } else { RATIO_GUESS };
    let mode = match (loose.len() > 64, a.len() > 0) {
        (_, false) if loose.is_empty() => "none",
        (_, false) => "loose",
        (false, true) => "compact",
        (true, true) => "mixed",
    };
    Storage {
        mode: mode.into(),
        loose_files: loose.len() as u64,
        loose_bytes,
        archive_shaders: a.len() as u64,
        archive_bytes: a.disk_bytes,
        archive_raw_bytes: a.raw_bytes,
        free_estimate: (loose_bytes as f64 * (1.0 - 1.0 / ratio)) as u64,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-archive-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(d.join(SPIRV_REL)).unwrap();
        d
    }

    /// SPIR-V-like words: a header and repeated instructions, so it compresses as real modules do.
    fn module(seed: u32, words: usize) -> Vec<u8> {
        let mut w = vec![0x0723_0203u32, 0x0001_0500, 0, 100 + seed, 0];
        for i in 0..words {
            w.push(if i % 7 == 0 { seed.wrapping_mul(2654435761).wrapping_add(i as u32) } else { 0x0004_003b + (i as u32 % 5) });
        }
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    fn put(game: &Path, i: u32) -> (String, Vec<u8>) {
        let h = format!("{:016x}", 0xabc0_0000u64 + i as u64);
        let b = module(i, 300 + (i as usize % 50) * 20);
        fs::write(game.join(SPIRV_REL).join(format!("{}.spv", h)), &b).unwrap();
        (h, b)
    }

    #[test]
    fn compact_then_read_back_then_expand() {
        let game = temp("roundtrip");
        let shaders: Vec<_> = (0..300).map(|i| put(&game, i)).collect();
        fs::write(game.join(SPIRV_REL).join("results.tsv"), "hash\tresources\n").unwrap();
        let before = storage(&game);
        assert_eq!(before.mode, "loose");
        assert_eq!(before.loose_files, 300);

        let (moved, finished) = compact(&game, None, &AtomicBool::new(false), |_, _| {}).unwrap();
        assert!(finished);
        assert_eq!(moved.shaders, 300);
        assert!(moved.disk_bytes < moved.raw_bytes, "{} >= {}", moved.disk_bytes, moved.raw_bytes);
        let after = storage(&game);
        assert_eq!((after.mode.as_str(), after.loose_files, after.archive_shaders), ("compact", 0, 300));
        assert!(game.join(SPIRV_REL).join("results.tsv").is_file(), "results.tsv stays loose");

        let a = Archive::open(&game);
        let spirv = game.join(SPIRV_REL);
        for (h, b) in &shaders {
            assert!(has_translation(&spirv, &a, h));
            assert_eq!(read_translation(&spirv, &a, h).as_deref(), Some(b.as_slice()));
        }

        // a shader translated again is written loose and wins; the next compact puts it in a newer segment, which wins
        let (h0, _) = &shaders[0];
        let newer = module(9999, 400);
        fs::write(spirv.join(format!("{}.spv", h0)), &newer).unwrap();
        assert_eq!(read_translation(&spirv, &Archive::open(&game), h0), Some(newer.clone()));
        compact(&game, None, &AtomicBool::new(false), |_, _| {}).unwrap();
        assert!(!spirv.join(format!("{}.spv", h0)).exists());
        assert_eq!(Archive::open(&game).read(h0), Some(newer.clone()));

        assert!(expand(&game, &AtomicBool::new(false), |_, _| {}).unwrap());
        assert!(!exists(&game));
        assert_eq!(fs::read(spirv.join(format!("{}.spv", h0))).unwrap(), newer);
        for (h, b) in &shaders[1..] {
            assert_eq!(&fs::read(spirv.join(format!("{}.spv", h))).unwrap(), b);
        }
        let _ = fs::remove_dir_all(&game);
    }

    #[test]
    fn cancelled_compact_keeps_everything_readable() {
        let game = temp("cancel");
        let shaders: Vec<_> = (0..600).map(|i| put(&game, i)).collect();
        let stop = AtomicBool::new(false);
        let (moved, finished) = compact(&game, None, &stop, |done, _| {
            if done >= 250 {
                stop.store(true, Ordering::SeqCst);
            }
        })
        .unwrap();
        assert!(!finished);
        assert_eq!(moved.shaders, 250, "the shaders read before the pause are flushed into a segment");
        assert_eq!(loose_files(&game).len(), 350);
        let a = Archive::open(&game);
        let spirv = game.join(SPIRV_REL);
        assert_eq!(a.len() as u64, moved.shaders);
        for (h, b) in &shaders {
            assert_eq!(read_translation(&spirv, &a, h).as_deref(), Some(b.as_slice()), "{}", h);
        }
        let _ = fs::remove_dir_all(&game);
    }

    #[test]
    fn segment_layout_matches_the_runtime() {
        // runtime nvn_shaders.cpp: PackHeader {magic[8], version, count, index_offset, reserved} = 32 bytes,
        // PackDiskEntry {hash[16], line_hash, offset, csize, usize} = 40 bytes, index at the end of the file
        let game = temp("layout");
        let (h, b) = put(&game, 7);
        compact(&game, None, &AtomicBool::new(false), |_, _| {}).unwrap();
        let (_, path) = segments(&archive_dir(&game)).pop().unwrap();
        let f = fs::read(&path).unwrap();
        assert_eq!(&f[..8], b"NXSPACK1");
        assert_eq!(u32_at(&f, 8), 1);
        assert_eq!(u32_at(&f, 12), 1);
        let index = u64_at(&f, 16) as usize;
        assert_eq!(index + 40, f.len());
        assert_eq!(&f[index..index + 16], h.as_bytes());
        let (off, csize, usize) = (u64_at(&f, index + 24) as usize, u32_at(&f, index + 32) as usize, u32_at(&f, index + 36) as usize);
        assert_eq!(off, 32);
        assert_eq!(usize, b.len());
        assert_eq!(zstd::bulk::decompress(&f[off..off + csize], usize).unwrap(), b);
        let _ = fs::remove_dir_all(&game);
    }
}
