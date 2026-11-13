// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin: files and paths. Paths are UTF-8 (A functions) or wchar_t (W functions, UTF-32 on macOS); backslashes are taken as separators.
#pragma once
#include "nxwin/handles.h"

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define FILE_READ_DATA 0x0001
#define FILE_WRITE_DATA 0x0002
#define FILE_APPEND_DATA 0x0004
#define FILE_READ_ATTRIBUTES 0x0080
#define FILE_WRITE_ATTRIBUTES 0x0100
#define DELETE 0x00010000
#define FILE_SHARE_READ 0x1
#define FILE_SHARE_WRITE 0x2
#define FILE_SHARE_DELETE 0x4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_SYSTEM 0x4
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define FILE_FLAG_NO_BUFFERING 0x20000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_COPY_ALLOWED 0x2
#define MOVEFILE_WRITE_THROUGH 0x8
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_DISK 1
#define FILE_TYPE_CHAR 2
#define FILE_TYPE_PIPE 3

typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow,
    nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

namespace nxwin {
int fd_of(HANDLE h);                                 // -1 when h is not a file
}

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl);
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newpos, DWORD method);
DWORD SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method);
BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
DWORD GetFileSize(HANDLE h, LPDWORD hi);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
DWORD GetFileType(HANDLE h);
BOOL GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info);
BOOL GetFileTime(HANDLE h, LPFILETIME create, LPFILETIME access, LPFILETIME write);
HANDLE GetStdHandle(DWORD which);
DWORD GetFileAttributesA(LPCSTR path);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID info);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID info);
BOOL SetFileAttributesA(LPCSTR path, DWORD attrs);
BOOL SetFileAttributesW(LPCWSTR path, DWORD attrs);
BOOL DeleteFileA(LPCSTR path);
BOOL DeleteFileW(LPCWSTR path);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL MoveFileA(LPCSTR from, LPCSTR to);
BOOL MoveFileW(LPCWSTR from, LPCWSTR to);
BOOL ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, LPVOID, LPVOID);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);
BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL RemoveDirectoryW(LPCWSTR path);
DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR buf, LPSTR* file_part);
DWORD GetFullPathNameW(LPCWSTR path, DWORD n, LPWSTR buf, LPWSTR* file_part);
DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf);
DWORD GetCurrentDirectoryW(DWORD n, LPWSTR buf);
BOOL SetCurrentDirectoryA(LPCSTR path);
BOOL SetCurrentDirectoryW(LPCWSTR path);
DWORD GetTempPathA(DWORD n, LPSTR buf);
DWORD GetTempPathW(DWORD n, LPWSTR buf);
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data);
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data);
BOOL FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data);
BOOL FindClose(HANDLE h);
BOOL GetDiskFreeSpaceExA(LPCSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free_total);
BOOL GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free_total);
#define WIN32_FIND_DATA WIN32_FIND_DATAW

typedef enum _FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic, FindExInfoMaxInfoLevel } FINDEX_INFO_LEVELS;
typedef enum _FINDEX_SEARCH_OPS { FindExSearchNameMatch, FindExSearchLimitToDirectories, FindExSearchLimitToDevices } FINDEX_SEARCH_OPS;
#define FIND_FIRST_EX_CASE_SENSITIVE 0x1
#define FIND_FIRST_EX_LARGE_FETCH 0x2
inline HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    return FindFirstFileW(pattern, static_cast<LPWIN32_FIND_DATAW>(data));
}
inline HANDLE FindFirstFileExA(LPCSTR pattern, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    return FindFirstFileA(pattern, static_cast<LPWIN32_FIND_DATAA>(data));
}
