// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: files, directories, paths.
#include "nxwin_internal.h"

#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>

namespace nxwin {

std::string fix_path(const char* p) {
    if (!p) return {};
    std::string s(p);
    for (auto& c : s)
        if (c == '\\') c = '/';
    // "C:/..." style absolute paths have no meaning here; "./" relative and "/" absolute paths pass through.
    return s;
}

namespace {
struct FileObj final : Object {
    int fd = -1;
    bool own = true;
    bool append = false;
    std::string path;
    bool delete_on_close = false;
    ~FileObj() override {
        if (fd >= 0 && own) ::close(fd);
        if (delete_on_close && !path.empty()) ::unlink(path.c_str());
    }
};
struct FindObj final : Object {
    DIR* dir = nullptr;
    std::string base, pattern;
    ~FindObj() override {
        if (dir) closedir(dir);
    }
};

void filetime_from_timespec(const struct timespec& ts, LPFILETIME ft) {
    const uint64_t v = static_cast<uint64_t>(ts.tv_sec) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100ull + 116444736000000000ull;
    ft->dwLowDateTime = static_cast<DWORD>(v);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
}
DWORD attrs_of(const struct stat& st, const std::string& name) {
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    const size_t slash = name.find_last_of('/');
    if (name.size() > slash + 1 && name[slash + 1] == '.') a |= FILE_ATTRIBUTE_HIDDEN;
    if (S_ISLNK(st.st_mode)) a |= FILE_ATTRIBUTE_REPARSE_POINT;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}
FileObj* stdio_obj(int fd) {
    static FileObj* objs[3] = {};
    if (!objs[fd]) {
        auto* f = new FileObj;
        f->fd = fd;
        f->own = false;
        f->refs.store(1 << 30);       // never freed by CloseHandle
        objs[fd] = f;
    }
    return objs[fd];
}
template <typename FD>
void fill_find(const std::string& path, const char* name, FD* data) {
    struct stat st{};
    const std::string full = path + "/" + name;
    stat(full.c_str(), &st);
    data->dwFileAttributes = attrs_of(st, full);
    filetime_from_timespec(st.st_birthtimespec, &data->ftCreationTime);
    filetime_from_timespec(st.st_atimespec, &data->ftLastAccessTime);
    filetime_from_timespec(st.st_mtimespec, &data->ftLastWriteTime);
    data->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    data->nFileSizeLow = static_cast<DWORD>(st.st_size);
    data->cAlternateFileName[0] = 0;
}
}  // namespace

int fd_of(HANDLE h) {
    auto* f = as<FileObj>(h);
    return f ? f->fd : -1;
}

}  // namespace nxwin

using namespace nxwin;

HANDLE CreateFileA(LPCSTR path_in, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disp, DWORD flags, HANDLE) {
    const std::string path = fix_path(path_in);
    int o = O_CLOEXEC;
    const bool rd = access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA);
    const bool wr = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
    o |= rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
        case CREATE_NEW: o |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: o |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: o |= O_CREAT; break;
        case TRUNCATE_EXISTING: o |= O_TRUNC; break;
        default: break;
    }
    struct stat st{};
    const bool existed = ::stat(path.c_str(), &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
        o = O_RDONLY | O_CLOEXEC;
    }
    const int fd = ::open(path.c_str(), o, 0644);
    if (fd < 0) {
        if (errno == EEXIST && disp == CREATE_NEW) SetLastError(ERROR_FILE_EXISTS);
        else set_errno_error();
        return INVALID_HANDLE_VALUE;
    }
    auto* f = new FileObj;
    f->fd = fd;
    f->path = path;
    f->append = (access & FILE_APPEND_DATA) && !(access & (GENERIC_WRITE | FILE_WRITE_DATA));
    f->delete_on_close = flags & FILE_FLAG_DELETE_ON_CLOSE;
    if (flags & FILE_FLAG_NO_BUFFERING) fcntl(fd, F_NOCACHE, 1);
    // Win32 reports ERROR_ALREADY_EXISTS on success when CREATE_ALWAYS / OPEN_ALWAYS found the file.
    SetLastError(existed && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return handle_of(f);
}
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    return CreateFileA(narrow(path).c_str(), access, share, sa, disp, flags, tmpl);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    const int fd = fd_of(h);
    if (got) *got = 0;
    if (fd < 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    size_t done = 0;
    while (done < n) {
        ssize_t r = ov ? ::pread(fd, static_cast<char*>(buf) + done, n - done,
                                 static_cast<off_t>(((static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset) + done))
                       : ::read(fd, static_cast<char*>(buf) + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            set_errno_error();
            if (got) *got = static_cast<DWORD>(done);
            return FALSE;
        }
        if (r == 0) break;
        done += static_cast<size_t>(r);
    }
    if (got) *got = static_cast<DWORD>(done);
    return TRUE;
}
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov) {
    auto* f = as<FileObj>(h);
    if (put) *put = 0;
    if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (f->append) ::lseek(f->fd, 0, SEEK_END);
    size_t done = 0;
    while (done < n) {
        ssize_t r = ov ? ::pwrite(f->fd, static_cast<const char*>(buf) + done, n - done,
                                  static_cast<off_t>(((static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset) + done))
                       : ::write(f->fd, static_cast<const char*>(buf) + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            set_errno_error();
            if (put) *put = static_cast<DWORD>(done);
            return FALSE;
        }
        done += static_cast<size_t>(r);
    }
    if (put) *put = static_cast<DWORD>(done);
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newpos, DWORD method) {
    const int fd = fd_of(h);
    if (fd < 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const off_t r = ::lseek(fd, static_cast<off_t>(dist.QuadPart), method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) { set_errno_error(); return FALSE; }
    if (newpos) newpos->QuadPart = r;
    return TRUE;
}
DWORD SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method) {
    LARGE_INTEGER d, n;
    d.QuadPart = hi ? ((static_cast<LONGLONG>(*hi) << 32) | static_cast<DWORD>(lo)) : lo;
    if (!SetFilePointerEx(h, d, &n, method)) return INVALID_SET_FILE_POINTER;
    if (hi) *hi = static_cast<LONG>(n.QuadPart >> 32);
    return static_cast<DWORD>(n.QuadPart);
}
BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
    const int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    size->QuadPart = st.st_size;
    return TRUE;
}
DWORD GetFileSize(HANDLE h, LPDWORD hi) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) return INVALID_FILE_SIZE;
    if (hi) *hi = static_cast<DWORD>(s.QuadPart >> 32);
    return static_cast<DWORD>(s.QuadPart);
}
BOOL SetEndOfFile(HANDLE h) {
    const int fd = fd_of(h);
    if (fd < 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const off_t pos = ::lseek(fd, 0, SEEK_CUR);
    if (pos < 0 || ::ftruncate(fd, pos) != 0) { set_errno_error(); return FALSE; }
    return TRUE;
}
// fsync on macOS does not reach the platter: F_FULLFSYNC does, which is what FlushFileBuffers promises.
BOOL FlushFileBuffers(HANDLE h) {
    const int fd = fd_of(h);
    if (fd < 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0) { set_errno_error(); return FALSE; }
    return TRUE;
}
DWORD GetFileType(HANDLE h) {
    const int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return FILE_TYPE_UNKNOWN;
    if (S_ISCHR(st.st_mode)) return FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return FILE_TYPE_PIPE;
    return FILE_TYPE_DISK;
}
BOOL GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info) {
    auto* f = as<FileObj>(h);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    std::memset(info, 0, sizeof *info);
    info->dwFileAttributes = attrs_of(st, f->path);
    filetime_from_timespec(st.st_birthtimespec, &info->ftCreationTime);
    filetime_from_timespec(st.st_atimespec, &info->ftLastAccessTime);
    filetime_from_timespec(st.st_mtimespec, &info->ftLastWriteTime);
    info->dwVolumeSerialNumber = static_cast<DWORD>(st.st_dev);
    info->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    info->nFileSizeLow = static_cast<DWORD>(st.st_size);
    info->nNumberOfLinks = static_cast<DWORD>(st.st_nlink);
    info->nFileIndexHigh = static_cast<DWORD>(st.st_ino >> 32);
    info->nFileIndexLow = static_cast<DWORD>(st.st_ino);
    return TRUE;
}
BOOL GetFileTime(HANDLE h, LPFILETIME create, LPFILETIME access, LPFILETIME write) {
    const int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (create) filetime_from_timespec(st.st_birthtimespec, create);
    if (access) filetime_from_timespec(st.st_atimespec, access);
    if (write) filetime_from_timespec(st.st_mtimespec, write);
    return TRUE;
}
HANDLE GetStdHandle(DWORD which) {
    if (which == STD_INPUT_HANDLE) return handle_of(stdio_obj(0));
    if (which == STD_OUTPUT_HANDLE) return handle_of(stdio_obj(1));
    if (which == STD_ERROR_HANDLE) return handle_of(stdio_obj(2));
    return INVALID_HANDLE_VALUE;
}

DWORD GetFileAttributesA(LPCSTR p) {
    const std::string path = fix_path(p);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) { set_errno_error(); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(st, path);
}
DWORD GetFileAttributesW(LPCWSTR p) { return GetFileAttributesA(narrow(p).c_str()); }
BOOL GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS, LPVOID out) {
    const std::string path = fix_path(p);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) { set_errno_error(); return FALSE; }
    auto* d = static_cast<LPWIN32_FILE_ATTRIBUTE_DATA>(out);
    d->dwFileAttributes = attrs_of(st, path);
    filetime_from_timespec(st.st_birthtimespec, &d->ftCreationTime);
    filetime_from_timespec(st.st_atimespec, &d->ftLastAccessTime);
    filetime_from_timespec(st.st_mtimespec, &d->ftLastWriteTime);
    d->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    d->nFileSizeLow = static_cast<DWORD>(st.st_size);
    return TRUE;
}
BOOL GetFileAttributesExW(LPCWSTR p, GET_FILEEX_INFO_LEVELS l, LPVOID out) { return GetFileAttributesExA(narrow(p).c_str(), l, out); }
BOOL SetFileAttributesA(LPCSTR p, DWORD attrs) {
    const std::string path = fix_path(p);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) { set_errno_error(); return FALSE; }
    mode_t m = st.st_mode & 07777;
    m = (attrs & FILE_ATTRIBUTE_READONLY) ? (m & ~static_cast<mode_t>(0222)) : (m | S_IWUSR);
    return ::chmod(path.c_str(), m) == 0;
}
BOOL SetFileAttributesW(LPCWSTR p, DWORD attrs) { return SetFileAttributesA(narrow(p).c_str(), attrs); }
BOOL DeleteFileA(LPCSTR p) {
    if (::unlink(fix_path(p).c_str()) != 0) { set_errno_error(); return FALSE; }
    return TRUE;
}
BOOL DeleteFileW(LPCWSTR p) { return DeleteFileA(narrow(p).c_str()); }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    const std::string f = fix_path(from), t = fix_path(to);
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && ::lstat(t.c_str(), &st) == 0) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    if (::rename(f.c_str(), t.c_str()) == 0) return TRUE;
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED)) {
        if (copyfile(f.c_str(), t.c_str(), nullptr, COPYFILE_ALL) == 0 && ::unlink(f.c_str()) == 0) return TRUE;
    }
    set_errno_error();
    return FALSE;
}
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) { return MoveFileExA(narrow(from).c_str(), narrow(to).c_str(), flags); }
BOOL MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, MOVEFILE_COPY_ALLOWED); }
BOOL MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED); }
BOOL ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD, LPVOID, LPVOID) {
    const std::string a = fix_path_w(replaced), b = fix_path_w(replacement);
    if (backup) {
        const std::string c = fix_path_w(backup);
        if (copyfile(a.c_str(), c.c_str(), nullptr, COPYFILE_ALL) != 0) { set_errno_error(); return FALSE; }
    }
    if (::rename(b.c_str(), a.c_str()) != 0) { set_errno_error(); return FALSE; }
    return TRUE;
}
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    const std::string f = fix_path(from), t = fix_path(to);
    copyfile_flags_t fl = COPYFILE_ALL;
    if (fail_if_exists) fl |= COPYFILE_EXCL;
    if (copyfile(f.c_str(), t.c_str(), nullptr, fl) != 0) {
        if (errno == EEXIST) SetLastError(ERROR_FILE_EXISTS);
        else set_errno_error();
        return FALSE;
    }
    return TRUE;
}
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) { return CopyFileA(narrow(from).c_str(), narrow(to).c_str(), fail_if_exists); }
BOOL CreateDirectoryA(LPCSTR p, LPSECURITY_ATTRIBUTES) {
    if (::mkdir(fix_path(p).c_str(), 0755) != 0) { set_errno_error(); return FALSE; }
    return TRUE;
}
BOOL CreateDirectoryW(LPCWSTR p, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryA(narrow(p).c_str(), sa); }
BOOL RemoveDirectoryA(LPCSTR p) {
    if (::rmdir(fix_path(p).c_str()) != 0) { set_errno_error(); return FALSE; }
    return TRUE;
}
BOOL RemoveDirectoryW(LPCWSTR p) { return RemoveDirectoryA(narrow(p).c_str()); }

namespace {
// Lexically normalised absolute path ("." and ".." resolved, like GetFullPathName; the file need not exist).
std::string full_path(const std::string& in) {
    std::string p = fix_path(in);
    if (p.empty() || p[0] != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        p = std::string(cwd) + "/" + p;
    }
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    std::string out;
    for (auto& s : parts) out += "/" + s;
    if (out.empty()) out = "/";
    if (p.size() > 1 && p.back() == '/' && out != "/") out += '/';
    return out;
}
}  // namespace
DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR buf, LPSTR* file_part) {
    const std::string f = full_path(path ? path : "");
    if (!buf || n <= f.size()) return static_cast<DWORD>(f.size() + 1);
    std::memcpy(buf, f.c_str(), f.size() + 1);
    if (file_part) {
        const size_t s = f.find_last_of('/');
        *file_part = s == std::string::npos || s + 1 >= f.size() ? nullptr : buf + s + 1;
    }
    return static_cast<DWORD>(f.size());
}
DWORD GetFullPathNameW(LPCWSTR path, DWORD n, LPWSTR buf, LPWSTR* file_part) {
    const std::wstring f = widen(full_path(narrow(path)).c_str());
    if (!buf || n <= f.size()) return static_cast<DWORD>(f.size() + 1);
    std::wmemcpy(buf, f.c_str(), f.size() + 1);
    if (file_part) {
        const size_t s = f.find_last_of(L'/');
        *file_part = s == std::wstring::npos || s + 1 >= f.size() ? nullptr : buf + s + 1;
    }
    return static_cast<DWORD>(f.size());
}
DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) { set_errno_error(); return 0; }
    const size_t k = std::strlen(cwd);
    if (!buf || n <= k) return static_cast<DWORD>(k + 1);
    std::memcpy(buf, cwd, k + 1);
    return static_cast<DWORD>(k);
}
DWORD GetCurrentDirectoryW(DWORD n, LPWSTR buf) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) { set_errno_error(); return 0; }
    const std::wstring w = widen(cwd);
    if (!buf || n <= w.size()) return static_cast<DWORD>(w.size() + 1);
    std::wmemcpy(buf, w.c_str(), w.size() + 1);
    return static_cast<DWORD>(w.size());
}
BOOL SetCurrentDirectoryA(LPCSTR p) {
    if (::chdir(fix_path(p).c_str()) != 0) { set_errno_error(); return FALSE; }
    return TRUE;
}
BOOL SetCurrentDirectoryW(LPCWSTR p) { return SetCurrentDirectoryA(narrow(p).c_str()); }
DWORD GetTempPathA(DWORD n, LPSTR buf) {
    const char* t = getenv("TMPDIR");
    std::string s = t && *t ? t : "/tmp/";
    if (s.back() != '/') s += '/';
    if (!buf || n <= s.size()) return static_cast<DWORD>(s.size() + 1);
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return static_cast<DWORD>(s.size());
}
DWORD GetTempPathW(DWORD n, LPWSTR buf) {
    char b[PATH_MAX];
    GetTempPathA(sizeof b, b);
    const std::wstring w = widen(b);
    if (!buf || n <= w.size()) return static_cast<DWORD>(w.size() + 1);
    std::wmemcpy(buf, w.c_str(), w.size() + 1);
    return static_cast<DWORD>(w.size());
}

// FindFirstFile: "dir/pattern" with * and ? in the last component.
namespace {
bool find_next(FindObj* f, std::string* name) {
    while (dirent* e = readdir(f->dir)) {
        if (fnmatch(f->pattern.c_str(), e->d_name, FNM_CASEFOLD) == 0) {
            *name = e->d_name;
            return true;
        }
    }
    return false;
}
FindObj* find_open(const std::string& pat_in, std::string* first) {
    const std::string pat = fix_path(pat_in);
    const size_t s = pat.find_last_of('/');
    auto* f = new FindObj;
    f->base = s == std::string::npos ? "." : (s == 0 ? "/" : pat.substr(0, s));
    f->pattern = s == std::string::npos ? pat : pat.substr(s + 1);
    if (f->pattern == "*.*") f->pattern = "*";
    f->dir = opendir(f->base.c_str());
    if (!f->dir || !find_next(f, first)) {
        SetLastError(f->dir ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
        f->unref();
        return nullptr;
    }
    return f;
}
}  // namespace
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
    std::string name;
    FindObj* f = find_open(pattern, &name);
    if (!f) return INVALID_HANDLE_VALUE;
    fill_find(f->base, name.c_str(), data);
    std::snprintf(data->cFileName, sizeof data->cFileName, "%s", name.c_str());
    return handle_of(f);
}
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
    std::string name;
    FindObj* f = find_open(narrow(pattern), &name);
    if (!f) return INVALID_HANDLE_VALUE;
    fill_find(f->base, name.c_str(), data);
    const std::wstring w = widen(name.c_str());
    const size_t k = std::min<size_t>(w.size(), MAX_PATH - 1);
    std::wmemcpy(data->cFileName, w.c_str(), k);
    data->cFileName[k] = 0;
    return handle_of(f);
}
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) {
    auto* f = as<FindObj>(h);
    std::string name;
    if (!f || !find_next(f, &name)) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    fill_find(f->base, name.c_str(), data);
    std::snprintf(data->cFileName, sizeof data->cFileName, "%s", name.c_str());
    return TRUE;
}
BOOL FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data) {
    auto* f = as<FindObj>(h);
    std::string name;
    if (!f || !find_next(f, &name)) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    fill_find(f->base, name.c_str(), data);
    const std::wstring w = widen(name.c_str());
    const size_t k = std::min<size_t>(w.size(), MAX_PATH - 1);
    std::wmemcpy(data->cFileName, w.c_str(), k);
    data->cFileName[k] = 0;
    return TRUE;
}
BOOL FindClose(HANDLE h) { return CloseHandle(h); }

BOOL GetDiskFreeSpaceExA(LPCSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free_total) {
    struct statfs s;
    if (::statfs(fix_path(dir && *dir ? dir : ".").c_str(), &s) != 0) { set_errno_error(); return FALSE; }
    if (avail) avail->QuadPart = static_cast<ULONGLONG>(s.f_bavail) * s.f_bsize;
    if (total) total->QuadPart = static_cast<ULONGLONG>(s.f_blocks) * s.f_bsize;
    if (free_total) free_total->QuadPart = static_cast<ULONGLONG>(s.f_bfree) * s.f_bsize;
    return TRUE;
}
BOOL GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free_total) {
    return GetDiskFreeSpaceExA(dir ? narrow(dir).c_str() : nullptr, avail, total, free_total);
}

HANDLE FindFirstFileW(const char* pattern, LPWIN32_FIND_DATAW data) { return FindFirstFileW(widen(pattern).c_str(), data); }
BOOL ReplaceFileW(const char* replaced, const char* replacement, const char* backup, DWORD flags, LPVOID a, LPVOID b) {
    const std::wstring r = widen(replaced), n = widen(replacement), k = backup ? widen(backup) : std::wstring();
    return ReplaceFileW(r.c_str(), n.c_str(), backup ? k.c_str() : nullptr, flags, a, b);
}
