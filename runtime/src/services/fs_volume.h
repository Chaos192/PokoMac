// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Host-folder file systems for fsp-srv (S3c): the volume behind an IFileSystem. Two kinds share one class:
//   * direct: reads and writes go straight to a folder (a read-only variant serves the loose RomFS folder as a file system);
//   * journalled (save data): the folder holds the COMMITTED state and is never written outside commit(). Writes land in a work folder
//     (copy on write: a file is copied there the first time it is opened for writing), deletions are recorded as tombstones, and every
//     read, existence test and listing sees the work folder over the committed one minus the tombstones. commit() is two-phase: (1) the
//     changed files are flushed and the list of changes is written to <root>.journal.<pid>, flushed and renamed to <root>.commit -- from
//     the rename on the commit is decided; (2) the changes are applied (deletions, folders, files moved over the committed ones) and the
//     marker is removed. A process killed in phase 1 leaves the committed state untouched; one killed in phase 2 leaves the marker and
//     the next open of the save finishes the apply (roll forward). Work folders and journals of dead processes are discarded.
//     A host failure while changing the work folder (Write / SetSize / Flush on a full disk, a file held by another program) taints the
//     transaction: the next commit() is refused, the committed folder stays as it was and the work folder is emptied (the game ignores
//     those results and commits anyway, which would turn a truncated file into the save). A failure in phase 1 itself refuses the commit
//     too. Refusals return a failure result (kUsableSpaceNotEnough for a full disk) and are counted for the player notice (fsapi).
// The idea (committed folder + copy-on-write work folder + tombstones + a decided-by-rename two-phase commit) is from smo-recomp
// runtime/fs/nnfs.c (GPL-3.0); the code is new.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace nxrt::services::fs {

// fs results (module 2: result = description << 9 | 2; https://switchbrew.org/wiki/Error_codes).
namespace rc {
constexpr uint32_t kPathNotFound = 0x202, kPathAlreadyExists = 0x402, kTargetLocked = 0xE02, kDirectoryNotEmpty = 0x1002;
constexpr uint32_t kTargetNotFound = 0x7D402, kPortSdCardNoDevice = 0xFA202, kNotImplemented = 0x177202, kOutOfRange = 0x177A02;
constexpr uint32_t kInvalidArgument = 0x2EE202, kTooLongPath = 0x2EE602, kNoAppend = 0x307202, kReadNotPermitted = 0x307402;
constexpr uint32_t kWriteNotPermitted = 0x307602, kReadOnlyFileSystem = 0x313802, kGenericFailure = 0x3E802;
constexpr uint32_t kUsableSpaceNotEnough = 0x3C02;
}  // namespace rc

enum OpenMode : uint32_t { kModeRead = 1, kModeWrite = 2, kModeAppend = 4 };

// An open file. Positioned I/O, safe to call from several threads.
class VFile {
public:
    virtual ~VFile() = default;
    virtual uint32_t read(int64_t offset, void* dst, uint64_t size, uint64_t* got) = 0;       // short at the end of the file
    virtual uint32_t write(int64_t offset, const void* src, uint64_t size) = 0;
    virtual uint32_t flush() = 0;
    virtual uint32_t set_size(int64_t size) = 0;
    virtual uint32_t get_size(int64_t* size) = 0;
};

struct DirItem {
    std::string name;
    bool is_dir = false;
    int64_t size = 0;
};

struct FileTimes {
    int64_t created = 0, modified = 0, accessed = 0;   // posix seconds
    bool valid = false;
};

class Volume {
public:
    // `root`: the folder (UTF-8). `journal`: save-data semantics (see above); `read_only`: every mutation answers kReadOnlyFileSystem.
    // `capacity`: bytes reported by GetTotalSpaceSize and enforced on growth for journalled volumes (0 = unlimited).
    Volume(const std::string& root, bool journal, bool read_only, uint64_t capacity);
    ~Volume();
    bool ok() const;                                    // the root exists (or was created) and recovery finished

    uint32_t create_file(const std::string& path, int64_t size);
    uint32_t delete_file(const std::string& path);
    uint32_t create_dir(const std::string& path);
    uint32_t delete_dir(const std::string& path);              // must be empty
    uint32_t delete_dir_recursive(const std::string& path);
    uint32_t clean_dir_recursive(const std::string& path);
    uint32_t rename_file(const std::string& from, const std::string& to);
    uint32_t rename_dir(const std::string& from, const std::string& to);
    uint32_t entry_type(const std::string& path, uint32_t* type);   // 0 directory, 1 file
    uint32_t open_file(const std::string& path, uint32_t mode, std::shared_ptr<VFile>* out);
    uint32_t list_dir(const std::string& path, std::vector<DirItem>* out);   // sorted by name
    uint32_t file_times(const std::string& path, FileTimes* out);
    uint32_t commit();                                  // journalled volumes; a no-op for direct ones
    uint64_t used_bytes();                              // files in the visible tree
    uint64_t capacity() const { return capacity_; }
    void set_capacity(uint64_t c) { capacity_ = c; }
    bool journalled() const { return journal_; }
    bool read_only() const { return read_only_; }

    // Applies a decided-but-unfinished commit of `root` and discards work folders / journals of processes that no longer run.
    // Called by the constructor of a journalled volume; exposed for tests (a simulated crash between the phases).
    static void recover(const std::string& root);
    // Test hook: when set to 1 commit() stops after phase 1 (the marker is written, nothing applied); 2 stops inside phase 2 after the
    // first applied change. The volume object is left as a crashed process would leave the folders.
    static void set_crash_point_for_tests(int point);
    // Test hook: the `nth` host operation at `point` from now on fails with the Win32 error `win32_error` (nth 0 = off). Process-wide.
    enum class IoPoint : int { kNone = 0, kWrite, kSetSize, kFlush, kCommitFlush, kDecide };
    static void set_io_failure_for_tests(IoPoint point, int nth, uint32_t win32_error);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    uint64_t capacity_;
    bool journal_, read_only_;
};

// Guest path hygiene: "/a/b/c" with single separators and no "." / ".." components, no trailing separator, no backslash or control
// characters, at most 0x300 bytes. Returns false with *err = a result code for a path that is not acceptable.
bool normalize_path(const char* raw, size_t max_len, std::string* out, uint32_t* err);

}  // namespace nxrt::services::fs
