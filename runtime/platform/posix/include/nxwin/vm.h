// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin: virtual memory (VirtualAlloc family over mach_vm), pagefile-backed sections (Mach named memory entries) and their views.
// Host pages are 16 KiB on Apple silicon: protection and commit changes cover whole host pages (a 4 KiB request widens to the
// 16 KiB page that holds it). Reservations and views made without an address start on 64 KiB boundaries like on Windows.
#pragma once
#include "nxwin/handles.h"

#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_RESET 0x00080000
#define MEM_TOP_DOWN 0x00100000
#define MEM_WRITE_WATCH 0x00200000
#define MEM_LARGE_PAGES 0x20000000
#define MEM_IMAGE 0x01000000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400
#define SEC_RESERVE 0x04000000
#define SEC_COMMIT 0x08000000
#define FILE_MAP_COPY 0x0001
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_EXECUTE 0x0020
#define FILE_MAP_ALL_ACCESS 0x000F001F

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _SYSTEM_INFO {
    union { DWORD dwOemId; struct { WORD wProcessorArchitecture; WORD wReserved; }; };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
#define PROCESSOR_ARCHITECTURE_AMD64 9
#define PROCESSOR_ARCHITECTURE_ARM64 12

typedef uint64_t DWORDLONG;
typedef ULONGLONG* PULONGLONG;
typedef struct _MEMORYSTATUSEX {
    DWORD dwLength, dwMemoryLoad;
    DWORDLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

namespace nxwin {
size_t host_page();                                   // 16384 on Apple silicon
// Reads n bytes at src into dst without faulting; false (and nothing guaranteed in dst) when any page is not readable.
bool safe_read(void* dst, const void* src, size_t n);
bool safe_write(void* dst, const void* src, size_t n);
// Holds: address space claimed early (inaccessible) so that nothing else in the process takes it. A fixed reservation or a section view
// that lies wholly inside a hold replaces the hold's pages (where Windows would see free space), and releasing it gives them back to the hold.
// Reserves every free part of [addr, addr + size); returns the bytes it could not take (already mapped by someone else). Safe to call from
// an early constructor (no dynamic initialisation involved).
uint64_t vm_hold(uint64_t addr, uint64_t size);
}  // namespace nxwin

LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
LPVOID VirtualAllocEx(HANDLE proc, LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old);
SIZE_T VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len);
BOOL VirtualLock(LPVOID addr, SIZE_T size);
BOOL VirtualUnlock(LPVOID addr, SIZE_T size);
BOOL FlushInstructionCache(HANDLE proc, LPCVOID addr, SIZE_T size);
HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD size_hi, DWORD size_lo, LPCSTR name);
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD size_hi, DWORD size_lo, LPCWSTR name);
LPVOID MapViewOfFile(HANDLE sec, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size);
LPVOID MapViewOfFileEx(HANDLE sec, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size, LPVOID base);
BOOL UnmapViewOfFile(LPCVOID view);
BOOL FlushViewOfFile(LPCVOID addr, SIZE_T size);
BOOL ReadProcessMemory(HANDLE proc, LPCVOID addr, LPVOID buf, SIZE_T n, SIZE_T* got);
BOOL WriteProcessMemory(HANDLE proc, LPVOID addr, LPCVOID buf, SIZE_T n, SIZE_T* put);
BOOL IsBadReadPtr(const void* p, UINT_PTR n);
BOOL IsBadWritePtr(LPVOID p, UINT_PTR n);
void GetSystemInfo(LPSYSTEM_INFO si);
void GetNativeSystemInfo(LPSYSTEM_INFO si);
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms);
BOOL GetPhysicallyInstalledSystemMemory(PULONGLONG kb);
