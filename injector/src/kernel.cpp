/*
 * starlight-sys :: injector/src/kernel.cpp
 *
 * Kernel context: finds System CR3, walks EPROCESS list,
 * reads process memory - all via physical memory IOCTLs.
 *
 * SAFETY:
 *   - Reads physical memory map from Windows registry BEFORE scanning
 *   - Only scans within verified RAM ranges (avoids MMIO -> no BSOD)
 *   - MSR read is optional (--skip-msr flag)
 */

#include "kernel.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <winternl.h>
#include <psapi.h>

/* NtQuerySystemInformation types for kernel base discovery */
typedef NTSTATUS (NTAPI *PFN_NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

#define SystemModuleInformation 11

#pragma pack(push, 1)
typedef struct _RTL_PROCESS_MODULE_INFORMATION {
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} RTL_PROCESS_MODULE_INFORMATION, *PRTL_PROCESS_MODULE_INFORMATION;

typedef struct _RTL_PROCESS_MODULES {
    ULONG NumberOfModules;
    RTL_PROCESS_MODULE_INFORMATION Modules[1];
} RTL_PROCESS_MODULES, *PRTL_PROCESS_MODULES;
#pragma pack(pop)

/*
 * CmResourceTypeMemory / Large memory flags.
 * Defined in winnt.h, but we define them here as fallback.
 */
#ifndef CmResourceTypeMemory
#define CmResourceTypeMemory 3
#endif
#ifndef CmResourceTypeMemoryLarge
#define CmResourceTypeMemoryLarge 7
#endif
#ifndef CM_RESOURCE_MEMORY_LARGE_40
#define CM_RESOURCE_MEMORY_LARGE_40 0x0200
#define CM_RESOURCE_MEMORY_LARGE_48 0x0400
#define CM_RESOURCE_MEMORY_LARGE_64 0x0800
#endif

/* ------------------------------------------------------------------ */
/*  Constructor / Destructor                                           */
/* ------------------------------------------------------------------ */

KernelContext::KernelContext(PhysicalMemory& phys)
    : m_phys(phys)
    , m_systemCR3(0)
    , m_systemEprocessPA(0)
    , m_systemEprocessVA(0)
    , m_ntoskrnlVA(0)
    , m_imageNameOffset(EPROCESS_IMAGENAME)
{
}

KernelContext::~KernelContext() {}

/* ------------------------------------------------------------------ */
/*  Load Physical Memory Map from Windows Registry                     */
/*                                                                     */
/*  Reads HKLM\HARDWARE\RESOURCEMAP\System Resources\Physical Memory   */
/*  \.Translated to get exact RAM ranges. This prevents scanning MMIO  */
/*  regions which can cause BSODs when MmMapIoSpace hits them.         */
/* ------------------------------------------------------------------ */

bool KernelContext::LoadMemoryMap()
{
    m_ramRanges.clear();

    HKEY hKey = NULL;
    LONG status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\RESOURCEMAP\\System Resources\\Physical Memory",
        0, KEY_READ, &hKey
    );

    if (status != ERROR_SUCCESS) {
        printf("[!] Cannot open memory map registry key (error %ld)\n", status);
        goto fallback;
    }

    {
        DWORD dataSize = 0;
        DWORD dataType = 0;
        status = RegQueryValueExW(hKey, L".Translated", NULL,
                                  &dataType, NULL, &dataSize);

        if (status != ERROR_SUCCESS || dataSize < 32) {
            RegCloseKey(hKey);
            printf("[!] Cannot query memory map size (error %ld)\n", status);
            goto fallback;
        }

        std::vector<uint8_t> data(dataSize);
        status = RegQueryValueExW(hKey, L".Translated", NULL,
                                  &dataType, data.data(), &dataSize);
        RegCloseKey(hKey);
        hKey = NULL;

        if (status != ERROR_SUCCESS) {
            printf("[!] Cannot read memory map data (error %ld)\n", status);
            goto fallback;
        }

        /*
         * Parse CM_RESOURCE_LIST.
         * These structures use #pragma pack(4) in winnt.h.
         *
         * CM_RESOURCE_LIST {
         *   ULONG Count;
         *   CM_FULL_RESOURCE_DESCRIPTOR List[1]; // variable
         * }
         *
         * We iterate through the partial resource descriptors looking
         * for Type == CmResourceTypeMemory (3).
         */
        uint8_t* ptr = data.data();
        uint8_t* dataEnd = ptr + dataSize;

        if (ptr + sizeof(ULONG) > dataEnd) goto fallback;
        ULONG fullCount = *reinterpret_cast<ULONG*>(ptr);
        ptr += sizeof(ULONG);  /* skip Count */

        for (ULONG i = 0; i < fullCount && ptr < dataEnd; i++) {
            /* CM_FULL_RESOURCE_DESCRIPTOR header:
             *   ULONG InterfaceType (4 bytes)
             *   ULONG BusNumber     (4 bytes)
             *   CM_PARTIAL_RESOURCE_LIST:
             *     USHORT Version    (2 bytes)
             *     USHORT Revision   (2 bytes)
             *     ULONG Count       (4 bytes)
             */
            if (ptr + 16 > dataEnd) break;

            ptr += 8;  /* skip InterfaceType + BusNumber */

            /* CM_PARTIAL_RESOURCE_LIST header */
            ptr += 4;  /* skip Version + Revision */
            ULONG partialCount = *reinterpret_cast<ULONG*>(ptr);
            ptr += 4;

            /* Iterate CM_PARTIAL_RESOURCE_DESCRIPTORs */
            for (ULONG j = 0; j < partialCount && ptr < dataEnd; j++) {
                /*
                 * CM_PARTIAL_RESOURCE_DESCRIPTOR (pack 4):
                 *   UCHAR  Type             offset 0
                 *   UCHAR  ShareDisposition  offset 1
                 *   USHORT Flags            offset 2
                 *   union u:               offset 4
                 *     Memory.Start (8 bytes) offset 4
                 *     Memory.Length (4 bytes) offset 12
                 *   Total union = max member size on x64
                 *
                 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) = 20 on x64
                 * But for safe parsing, we read fields manually.
                 */
                if (ptr + 16 > dataEnd) break;  /* minimum 16 bytes */

                uint8_t  type  = ptr[0];
                uint16_t flags = *reinterpret_cast<uint16_t*>(ptr + 2);

                /* Memory fields at offset 4 and 12 */
                uint64_t start  = *reinterpret_cast<uint64_t*>(ptr + 4);
                uint32_t length = *reinterpret_cast<uint32_t*>(ptr + 12);

                if (type == CmResourceTypeMemory ||
                    type == CmResourceTypeMemoryLarge)
                {
                    uint64_t actualLength = length;

                    /* Large memory descriptors scale the Length field */
                    if (flags & CM_RESOURCE_MEMORY_LARGE_40)
                        actualLength = static_cast<uint64_t>(length) << 8;
                    else if (flags & CM_RESOURCE_MEMORY_LARGE_48)
                        actualLength = static_cast<uint64_t>(length) << 16;
                    else if (flags & CM_RESOURCE_MEMORY_LARGE_64)
                        actualLength = static_cast<uint64_t>(length) << 32;

                    if (actualLength > 0 && start < 0xFFFFFFFFFULL) {
                        m_ramRanges.push_back({start, start + actualLength});
                    }
                }

                /*
                 * Advance to next descriptor.
                 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) on x64 = 20 bytes
                 * (4 header + 16 union, with pack 4)
                 */
                ptr += 20;
            }
        }
    }

    if (!m_ramRanges.empty()) {
        /* Sort by start address */
        std::sort(m_ramRanges.begin(), m_ramRanges.end(),
                  [](const MemRange& a, const MemRange& b) {
                      return a.start < b.start;
                  });

        uint64_t totalMB = 0;
        for (auto& r : m_ramRanges)
            totalMB += (r.end - r.start) / (1024 * 1024);

        printf("[+] Memory map: %zu ranges, %llu MB total RAM\n",
               m_ramRanges.size(), totalMB);

        for (auto& r : m_ramRanges) {
            printf("    0x%09llX - 0x%09llX (%llu MB)\n",
                   r.start, r.end,
                   (r.end - r.start) / (1024 * 1024));
        }
        return true;
    }

fallback:
    /*
     * Fallback: conservative ranges that should be safe on most systems.
     * These avoid the PCI MMIO hole (typically 0xC0000000-0xFFFFFFFF).
     */
    printf("[*] Using conservative fallback memory ranges\n");
    m_ramRanges.push_back({0x1000, 0xA0000});           /* 1KB - 640KB  */
    m_ramRanges.push_back({0x100000, 0x80000000ULL});   /* 1MB - 2GB    */
    return true;
}

/* ------------------------------------------------------------------ */
/*  PreloadSafetyFilter                                                */
/*  Call BEFORE loading driver or doing any physical reads.            */
/*  Loads RAM map from registry and installs PA filter.                */
/* ------------------------------------------------------------------ */

bool KernelContext::PreloadSafetyFilter()
{
    printf("[*] Loading physical memory map from registry...\n");
    fflush(stdout);

    if (!LoadMemoryMap()) {
        printf("[!] Failed to load memory map\n");
        fflush(stdout);
        return false;
    }

    m_phys.SetSafeRanges(m_ramRanges);
    printf("[+] PA safety filter active (%zu ranges)\n",
           m_ramRanges.size());
    fflush(stdout);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Initialize                                                         */
/* ------------------------------------------------------------------ */

bool KernelContext::Initialize(bool skipMSR)
{
    printf("[*] Initializing kernel context...\n");
    fflush(stdout);

    /*
     * Step 1: Ensure PA safety filter is loaded.
     * If PreloadSafetyFilter() was already called, ranges are present.
     * Otherwise load them now.
     */
    if (m_ramRanges.empty()) {
        if (!LoadMemoryMap()) {
            printf("[!] Failed to load memory map\n");
            fflush(stdout);
            return false;
        }
        m_phys.SetSafeRanges(m_ramRanges);
        printf("[+] PA safety filter active (%zu ranges)\n",
               m_ramRanges.size());
        fflush(stdout);
    } else {
        printf("[+] PA safety filter already active (%zu ranges)\n",
               m_ramRanges.size());
        fflush(stdout);
    }

    /*
     * Step 2: Get kernel base address.
     *
     * Strategy A: Read MSR LSTAR (if driver supports MSR and not skipped)
     * Strategy B: NtQuerySystemInformation (user-mode, always works)
     *
     * We try A first, fall back to B.
     */
    uint64_t lstar = 0;
    if (!skipMSR) {
        printf("[*] Reading MSR LSTAR...\n"); fflush(stdout);
        lstar = m_phys.ReadMSR(MSR_LSTAR);
        if (lstar != 0 && lstar > KERNEL_VA_MIN) {
            printf("[+] LSTAR = 0x%llX\n", lstar); fflush(stdout);
            m_ntoskrnlVA = lstar & ~0xFFFFFULL;
        } else {
            printf("[*] MSR read returned 0x%llX (not usable)\n", lstar);
            fflush(stdout);
            lstar = 0;
        }
    } else {
        printf("[*] MSR read skipped (backend/flag)\n"); fflush(stdout);
    }

    /* Fallback: get kernel base from user-mode API */
    if (lstar == 0) {
        printf("[*] Getting kernel base via NtQuerySystemInformation...\n");
        fflush(stdout);
        uint64_t kbase = GetKernelBaseFromUserMode();
        if (kbase > KERNEL_VA_MIN) {
            printf("[+] Kernel base = 0x%llX (user-mode API)\n", kbase);
            fflush(stdout);
            m_ntoskrnlVA = kbase;
            /* Use KiSystemCall64Shadow (typical LSTAR target) as proxy VA */
            lstar = kbase + 0x200000;  /* approximate offset */
        } else {
            printf("[*] User-mode kernel base query failed\n");
            fflush(stdout);
        }
    }

    /*
     * Step 3: Try fast CR3 brute-force (first 16MB only).
     */
    if (lstar > KERNEL_VA_MIN) {
        if (FindSystemCR3(lstar)) {
            printf("[+] System CR3 = 0x%llX (fast brute-force)\n",
                   m_systemCR3);
            fflush(stdout);
        }
    }

    /*
     * Step 3.5: PRIMARY path - resolve PsInitialSystemProcess export.
     *
     * If CR3 brute-force succeeded and we have the kernel base,
     * we can find System EPROCESS through ntoskrnl.exe exports.
     * This only needs ~20 physical reads (vs ~268M for full scan
     * with ThrottleStop). If this succeeds, skip the heavy scan.
     */
    bool haveEprocess = false;
    if (m_systemCR3 != 0 && m_ntoskrnlVA != 0) {
        haveEprocess = FindSystemEprocessViaExport();
        if (haveEprocess) {
            printf("[+] EPROCESS found via export (no scan needed)\n");
            fflush(stdout);
            DetectImageNameOffset();
        } else {
            printf("[*] Export resolution failed, falling back to scan\n");
            fflush(stdout);
        }
    }

    /*
     * Step 4: Fallback - scan physical memory for System EPROCESS.
     * Only runs if export resolution failed.
     * - Limited to first 512MB (System EPROCESS is always there)
     * - Backend-aware: sparse probing for small-chunk backends
     * - PA safety filter rejects any out-of-range addresses
     * - Rate-limited to reduce driver stress
     */
    if (!haveEprocess) {
        printf("[*] Starting EPROCESS scan (fallback, up to 4 GB)...\n");
        printf("[*] May take 5-15 min. MSR/NtQSI failed - using physical scan.\n");
        fflush(stdout);
        if (!ScanForSystemProcess()) {
            printf("[!] System EPROCESS not found\n"); fflush(stdout);
            return false;
        }
    }

    printf("[+] System CR3 = 0x%llX\n", m_systemCR3); fflush(stdout);

    /*
     * Step 5: Validate CR3 (only if LSTAR available).
     */
    if (lstar != 0 && lstar > KERNEL_VA_MIN) {
        printf("[*] Validating CR3...\n"); fflush(stdout);
        if (ValidateCR3(m_systemCR3, lstar)) {
            printf("[+] CR3 validated\n"); fflush(stdout);
        } else {
            printf("[*] CR3 validation inconclusive (continuing)\n");
            fflush(stdout);
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  CR3 Brute-Force (first 16MB, LSTAR validation)                     */
/*                                                                     */
/*  Tries every page-aligned address in the first 16MB as a potential  */
/*  System CR3 (PML4 base). Validates by translating the LSTAR VA     */
/*  through the candidate page tables and checking the result.         */
/*                                                                     */
/*  Why this is safe:                                                  */
/*  - First 16MB is always conventional RAM on x86-64                  */
/*  - PA safety filter protects intermediate page table reads          */
/*  - At most ~4000 reads (16MB / 4KB pages)                           */
/* ------------------------------------------------------------------ */

bool KernelContext::FindSystemCR3(uint64_t lstarVA)
{
    printf("[*] CR3 brute-force (first 16MB)...\n"); fflush(stdout);

    /* PML4 index for LSTAR's virtual address */
    uint64_t pml4_idx = (lstarVA >> 39) & 0x1FF;

    int checked = 0;
    int presentCount = 0;

    /*
     * Iterate page-aligned addresses in first 16MB.
     * System CR3 on Windows is typically < 4MB, but we scan up to 16MB
     * for safety margin.
     */
    for (uint64_t cr3 = 0x1000; cr3 < 0x1000000; cr3 += 0x1000) {
        checked++;

        /* Read the PML4 entry that maps LSTAR's VA range */
        uint64_t pml4e = m_phys.ReadPhys<uint64_t>(cr3 + pml4_idx * 8);

        /* Must be present */
        if (!(pml4e & PTE_PRESENT_BIT))
            continue;

        /* Frame address must be reasonable */
        uint64_t frame = pml4e & PTE_FRAME_MASK;
        if (frame == 0 || frame > 0x800000000ULL)  /* < 32GB */
            continue;

        presentCount++;

        /* Full 4-level page table walk to translate LSTAR */
        uint64_t pa = m_phys.VirtToPhys(cr3, lstarVA);
        if (pa == 0)
            continue;

        /* Read a few bytes at translated address - should be code */
        uint8_t code[8];
        if (!m_phys.ReadPhysical(pa, code, sizeof(code)))
            continue;

        /* Must not be all zeros or all 0xFF */
        bool allZero = true, allFF = true;
        for (int i = 0; i < 8; i++) {
            if (code[i] != 0x00) allZero = false;
            if (code[i] != 0xFF) allFF = false;
        }
        if (allZero || allFF)
            continue;

        /* Found a valid CR3! */
        m_systemCR3 = cr3;
        printf("[+] CR3 brute-force hit: 0x%llX "
               "(checked %d pages, %d present)\n",
               cr3, checked, presentCount);
        fflush(stdout);
        return true;
    }

    printf("[*] CR3 brute-force: no match "
           "(%d pages, %d present)\n", checked, presentCount);
    fflush(stdout);
    return false;
}

/* ------------------------------------------------------------------ */
/*  Find System EPROCESS via PsInitialSystemProcess Export              */
/*                                                                     */
/*  PRIMARY method - avoids the heavy physical memory scan entirely.    */
/*                                                                     */
/*  1. Load ntoskrnl.exe in user-mode (PE only, DONT_RESOLVE_DLL_REFERENCES) */
/*  2. GetProcAddress("PsInitialSystemProcess") → user-mode addr       */
/*  3. Compute RVA, add to kernel base → kernel VA of the pointer      */
/*  4. VirtToPhys(CR3, ptr_VA) + read 8 bytes → EPROCESS VA            */
/*  5. Read PID + DTB from EPROCESS to validate                        */
/*                                                                     */
/*  Total physical reads: ~20 (page-table walks + value reads).        */
/*  This replaces a 268-million-IOCTL scan on ThrottleStop.            */
/* ------------------------------------------------------------------ */

bool KernelContext::FindSystemEprocessViaExport()
{
    if (m_systemCR3 == 0 || m_ntoskrnlVA == 0)
        return false;

    printf("[*] Resolving PsInitialSystemProcess export...\n");
    fflush(stdout);

    /*
     * Step 1: Load ntoskrnl.exe into user-mode address space.
     * DONT_RESOLVE_DLL_REFERENCES: maps the PE image without executing
     * DllMain or resolving imports. Safe and fast.
     */
    HMODULE hNt = LoadLibraryExA(
        "ntoskrnl.exe", NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!hNt) {
        printf("[*] LoadLibraryEx(ntoskrnl.exe) failed (error %lu)\n",
               GetLastError());
        fflush(stdout);
        return false;
    }

    /*
     * Step 2: Find the PsInitialSystemProcess export.
     * This is an EPROCESS** (pointer to the System EPROCESS pointer).
     * It has been a stable export since Windows XP.
     */
    auto pUser = reinterpret_cast<uint8_t*>(
        GetProcAddress(hNt, "PsInitialSystemProcess"));
    if (!pUser) {
        printf("[*] PsInitialSystemProcess export not found\n");
        fflush(stdout);
        FreeLibrary(hNt);
        return false;
    }

    /*
     * Step 3: Calculate the kernel VA of PsInitialSystemProcess.
     * RVA = user_addr - user_base
     * kernel_VA = ntoskrnl_base + RVA
     */
    uint64_t rva   = static_cast<uint64_t>(pUser - reinterpret_cast<uint8_t*>(hNt));
    uint64_t ptrVA = m_ntoskrnlVA + rva;
    FreeLibrary(hNt);
    hNt = NULL;

    printf("[*] PsInitialSystemProcess VA = 0x%llX (RVA 0x%llX)\n",
           ptrVA, rva);
    fflush(stdout);

    /*
     * Step 4: Read the EPROCESS pointer through the page tables.
     * VirtToPhys translates the kernel VA → PA, then we read 8 bytes.
     * This takes ~5 physical reads (4-level page walk + 1 value read).
     */
    uint64_t eprocessVA = m_phys.ReadVirt<uint64_t>(m_systemCR3, ptrVA);
    if (eprocessVA == 0 || eprocessVA < KERNEL_VA_MIN) {
        printf("[*] PsInitialSystemProcess read failed or invalid "
               "(0x%llX)\n", eprocessVA);
        fflush(stdout);
        return false;
    }

    printf("[*] System EPROCESS VA = 0x%llX\n", eprocessVA);
    fflush(stdout);

    /*
     * Step 5: Validate by reading PID and DTB from the EPROCESS.
     * Each read is another ~5 physical reads (page walk + value).
     */
    uint64_t pid = m_phys.ReadVirt<uint64_t>(
        m_systemCR3, eprocessVA + EPROCESS_PID);
    uint64_t dtb = m_phys.ReadVirt<uint64_t>(
        m_systemCR3, eprocessVA + EPROCESS_DTB);

    if (pid != 4) {
        printf("[*] PID mismatch: expected 4, got %llu\n", pid);
        fflush(stdout);
        return false;
    }

    /*
     * Step 6: Translate EPROCESS VA → PA for later use
     * (FindProcess reads EPROCESS fields from PA).
     */
    uint64_t eprocessPA = m_phys.VirtToPhys(m_systemCR3, eprocessVA);
    if (eprocessPA == 0) {
        printf("[*] EPROCESS VA→PA translation failed\n");
        fflush(stdout);
        return false;
    }

    /* Success - store results */
    m_systemEprocessVA = eprocessVA;
    m_systemEprocessPA = eprocessPA;

    /* Refine CR3 from actual EPROCESS DTB (more reliable than brute-force) */
    if (dtb != 0 && (dtb & 0xFFF) == 0)
        m_systemCR3 = dtb;

    printf("[+] System EPROCESS via export: PA=0x%llX VA=0x%llX "
           "PID=%llu DTB=0x%llX\n",
           eprocessPA, eprocessVA, pid, dtb);
    fflush(stdout);

    return true;
}

/* ------------------------------------------------------------------ */
/*  Scan SAFE Physical Memory for System EPROCESS (PID=4)              */
/*                                                                     */
/*  FALLBACK method - only used when export resolution fails.          */
/*  ONLY scans within m_ramRanges (from registry).                     */
/*  This prevents hitting MMIO regions that could BSOD.                */
/*                                                                     */
/*  Backend-aware:                                                     */
/*  - Large chunk (>= 4KB): reads full pages (1 IOCTL/page)           */
/*  - Small chunk (< 4KB, e.g. ThrottleStop 8B): sparse probing,      */
/*    reads only the 8-byte PID field per candidate offset.            */
/* ------------------------------------------------------------------ */

bool KernelContext::ScanForSystemProcess()
{
    if (m_ramRanges.empty()) {
        printf("[!] No memory ranges to scan\n");
        return false;
    }

    /*
     * Detect backend capabilities to choose scan strategy.
     * Large-chunk backends (>= 4KB): read full pages, scan in-memory.
     * Small-chunk backends (< 4KB, e.g. ThrottleStop 8B): sparse probing,
     * only read the 8-byte PID field at each candidate offset.
     */
    const size_t maxChunk = m_phys.GetBackend()->MaxReadChunk();
    const bool sparseMode = (maxChunk < 0x1000);

    printf("[*] Scanning RAM for System EPROCESS (limit %llu MB, %s)...\n",
           MAX_EPROCESS_SCAN_PA / (1024 * 1024),
           sparseMode ? "sparse-probe" : "full-page");
    fflush(stdout);

    constexpr size_t PAGE_SZ = 0x1000;
    uint64_t totalScanned = 0;
    uint64_t totalReads   = 0;
    uint64_t skippedPages = 0;
    int candidates = 0;

    /*
     * Full-page scan buffer - only used when sparseMode == false.
     * Allocate on stack only for large-chunk backends.
     */
    uint8_t page[PAGE_SZ];

    for (const auto& range : m_ramRanges) {
        /* Skip ranges below 1MB (unlikely to contain EPROCESS) */
        uint64_t scanStart = (std::max)(range.start,
                                        static_cast<uint64_t>(0x100000));

        /* Limit scan to MAX_EPROCESS_SCAN_PA (4 GB). */
        uint64_t scanEnd = (std::min)(range.end, MAX_EPROCESS_SCAN_PA);

        if (scanStart >= scanEnd)
            continue;

        /* Align to page boundary */
        scanStart = (scanStart + PAGE_SZ - 1) & ~(PAGE_SZ - 1);

        for (uint64_t pa = scanStart; pa < scanEnd; pa += PAGE_SZ) {

            if (sparseMode) {
                /*
                 * SPARSE MODE (ThrottleStop / small-chunk backends).
                 *
                 * Instead of reading the full 4KB page (which would need
                 * 512 IOCTLs with an 8-byte chunk size), we only read the
                 * 8-byte PID field at each candidate offset within the page.
                 *
                 * Cascade filter: only read more fields if PID matches.
                 * Since PID==4 is extremely rare in random memory, the vast
                 * majority of candidates are rejected after 1 read.
                 *
                 * IOCTLs per page: ~164 (one 8-byte read per candidate)
                 * vs. 512 for full-page mode. And most importantly, we can
                 * reject non-matches with just 1 IOCTL instead of 512.
                 */
                for (size_t off = 0;
                     off + EPROCESS_IMAGENAME + 16 <= PAGE_SZ;
                     off += 0x10)
                {
                    /* Read PID field (8 bytes = 1 IOCTL with ThrottleStop) */
                    uint64_t candidatePA = pa + off;
                    uint64_t pid = m_phys.ReadPhys<uint64_t>(
                        candidatePA + EPROCESS_PID);
                    totalReads++;

                    if (pid != 4)
                        continue;

                    /* PID match! Read DTB (1 more IOCTL) */
                    uint64_t dtb = m_phys.ReadPhys<uint64_t>(
                        candidatePA + EPROCESS_DTB);
                    totalReads++;

                    if (dtb == 0 || (dtb & 0xFFF) != 0)
                        continue;
                    if (dtb > 0x800000000ULL)
                        continue;
                    if (m_systemCR3 != 0 && dtb != m_systemCR3)
                        continue;

                    /* Read ImageFileName (8 bytes, check "System") */
                    char imgBuf[8] = {};
                    m_phys.ReadPhysical(
                        candidatePA + EPROCESS_IMAGENAME, imgBuf, 8);
                    totalReads++;

                    if (memcmp(imgBuf, "System", 6) != 0)
                        continue;

                    /* Read Flink (8 bytes, validate kernel VA) */
                    uint64_t flink = m_phys.ReadPhys<uint64_t>(
                        candidatePA + EPROCESS_LINKS);
                    totalReads++;

                    if (flink < KERNEL_VA_MIN)
                        continue;

                    /* Strong match */
                    candidates++;
                    printf("[+] Found System EPROCESS at PA 0x%llX\n",
                           candidatePA);
                    printf("    PID=%llu DTB=0x%llX Name=%.7s "
                           "(%llu reads)\n",
                           pid, dtb, imgBuf, totalReads);

                    m_systemCR3        = dtb;
                    m_systemEprocessPA = candidatePA;
                    return true;
                }

                totalScanned += PAGE_SZ;

                /*
                 * Rate limit for sparse mode:
                 * Sleep every 4 pages to let the driver breathe.
                 * 4 pages x ~256 checks/page = ~1024 IOCTLs per batch.
                 */
                if (((pa - scanStart) / PAGE_SZ) % 4 == 0 &&
                    pa > scanStart)
                    Sleep(1);

            } else {
                /*
                 * FULL-PAGE MODE (LnvMSRIO / large-chunk backends).
                 * 1 IOCTL per page - efficient, scan in-memory buffer.
                 */
                if (!m_phys.ReadPhysical(pa, page, PAGE_SZ)) {
                    skippedPages++;
                    continue;
                }

                totalScanned += PAGE_SZ;
                totalReads++;

                for (size_t off = 0;
                     off + EPROCESS_IMAGENAME + 16 <= PAGE_SZ;
                     off += 0x10)
                {
                    uint8_t* base = page + off;

                    uint64_t pid = *reinterpret_cast<uint64_t*>(
                        base + EPROCESS_PID);
                    if (pid != 4)
                        continue;

                    uint64_t dtb = *reinterpret_cast<uint64_t*>(
                        base + EPROCESS_DTB);
                    if (dtb == 0 || (dtb & 0xFFF) != 0)
                        continue;
                    if (dtb > 0x800000000ULL)
                        continue;
                    if (m_systemCR3 != 0 && dtb != m_systemCR3)
                        continue;

                    char* imgName = reinterpret_cast<char*>(
                        base + EPROCESS_IMAGENAME);
                    if (memcmp(imgName, "System", 6) != 0)
                        continue;

                    uint64_t flink = *reinterpret_cast<uint64_t*>(
                        base + EPROCESS_LINKS);
                    if (flink < KERNEL_VA_MIN)
                        continue;

                    /* Strong match */
                    candidates++;
                    uint64_t epa = pa + off;

                    printf("[+] Found System EPROCESS at PA 0x%llX\n",
                           epa);
                    printf("    PID=%llu DTB=0x%llX Name=%.15s\n",
                           pid, dtb, imgName);

                    m_systemCR3        = dtb;
                    m_systemEprocessPA = epa;
                    return true;
                }

                /*
                 * Rate limit for full-page mode:
                 * Sleep every 128 pages (~512KB) to reduce driver stress.
                 */
                if (((pa - scanStart) & 0x7FFFF) == 0 && pa > scanStart)
                    Sleep(1);
            }

            /* Progress every 16 MB (more frequent for earlier feedback) */
            if ((totalScanned & 0xFFFFFF) == 0 && totalScanned > 0) {
                printf("[*] Scanned %llu MB (%llu reads, "
                       "%llu pages skipped)...\n",
                       totalScanned / (1024 * 1024),
                       totalReads, skippedPages);
                fflush(stdout);
            }
        }
    }

    printf("[!] Not found after scanning %llu MB, %llu reads, "
           "%llu pages skipped (%d candidates)\n",
           totalScanned / (1024 * 1024), totalReads,
           skippedPages, candidates);
    return false;
}

/* ------------------------------------------------------------------ */
/*  Validate CR3                                                       */
/* ------------------------------------------------------------------ */

bool KernelContext::ValidateCR3(uint64_t cr3, uint64_t knownVA)
{
    uint64_t pa = m_phys.VirtToPhys(cr3, knownVA);
    if (pa == 0)
        return false;

    uint8_t buf[16];
    if (!m_phys.ReadPhysical(pa, buf, sizeof(buf)))
        return false;

    bool allZero = true, allFF = true;
    for (int i = 0; i < 16; i++) {
        if (buf[i] != 0x00) allZero = false;
        if (buf[i] != 0xFF) allFF = false;
    }

    return !allZero && !allFF;
}

/* ------------------------------------------------------------------ */
/*  Auto-detect ImageFileName offset within EPROCESS                   */
/*                                                                     */
/*  The ImageFileName offset varies between Windows builds.            */
/*  We probe the known System EPROCESS (PID=4) for the string          */
/*  "System" at offsets 0x400-0x700 to find the correct location.      */
/*  This eliminates the need for per-build offset tables.              */
/* ------------------------------------------------------------------ */

bool KernelContext::DetectImageNameOffset()
{
    if (m_systemEprocessVA == 0 || m_systemCR3 == 0)
        return false;

    printf("[*] Auto-detecting ImageFileName offset...\n");
    fflush(stdout);

    /*
     * Read a large chunk of the System EPROCESS via virtual memory.
     * ImageFileName location varies by Windows build:
     *   - Win10/11 older: 0x5A8
     *   - Win11 26200+:   unknown, possibly > 0x700
     * We search 0x200-0xC00 (2.5 KB) to cover all known and future builds.
     * Read page-by-page to handle page boundaries correctly.
     */
    constexpr uint32_t PROBE_START = 0x200;
    constexpr uint32_t PROBE_END   = 0xC00;
    constexpr uint32_t PROBE_SIZE  = PROBE_END - PROBE_START;

    uint8_t buf[PROBE_SIZE] = {};

    /*
     * Read in page-sized chunks to avoid cross-page failures.
     * EPROCESS at VA 0x...AA040 → first page boundary at offset 0xFC0.
     */
    uint32_t bytesRead = 0;
    for (uint32_t chunk = 0; chunk < PROBE_SIZE; chunk += 0x1000) {
        uint32_t chunkSize = (std::min)(static_cast<uint32_t>(0x1000),
                                        PROBE_SIZE - chunk);
        if (m_phys.ReadVirtual(m_systemCR3,
                               m_systemEprocessVA + PROBE_START + chunk,
                               buf + chunk, chunkSize))
            bytesRead += chunkSize;
    }

    if (bytesRead == 0) {
        printf("[*] Failed to read EPROCESS range for offset detection\n");
        fflush(stdout);
        return false;
    }

    /* Dump bytes around the old hardcoded offset for diagnostics */
    uint32_t diagOff = EPROCESS_IMAGENAME - PROBE_START;
    if (diagOff + 16 <= PROBE_SIZE) {
        printf("[*] Bytes at old offset 0x%X: ", EPROCESS_IMAGENAME);
        for (int i = 0; i < 16; i++)
            printf("%02X ", buf[diagOff + i]);
        printf("\n");
        fflush(stdout);
    }

    /* Search byte-by-byte for "System\0" */
    for (uint32_t off = 0; off + 7 <= bytesRead; off++) {
        if (memcmp(buf + off, "System\0", 7) == 0) {
            uint32_t foundOffset = PROBE_START + off;
            printf("[+] ImageFileName found at offset 0x%X", foundOffset);
            if (foundOffset != EPROCESS_IMAGENAME) {
                printf(" (was 0x%X - BUILD OFFSET CHANGED!)\n",
                       EPROCESS_IMAGENAME);
            } else {
                printf(" (matches hardcoded)\n");
            }
            fflush(stdout);
            m_imageNameOffset = foundOffset;
            return true;
        }
    }

    printf("[!] ImageFileName 'System' not found in EPROCESS 0x%X-0x%X\n",
           PROBE_START, PROBE_END);
    printf("[*] Using default offset 0x%X (may produce garbage names)\n",
           EPROCESS_IMAGENAME);
    fflush(stdout);
    return false;
}

/* ------------------------------------------------------------------ */
/*  Find Process by Image Name                                         */
/* ------------------------------------------------------------------ */

bool KernelContext::FindProcess(const char* imageName, ProcessInfo& out)
{
    if (m_systemCR3 == 0) {
        printf("[!] Not initialized\n");
        return false;
    }

    printf("[*] Searching: %s\n", imageName);

    uint64_t listHead_flink = 0;
    m_phys.ReadPhysical(
        m_systemEprocessPA + EPROCESS_LINKS,
        &listHead_flink,
        sizeof(listHead_flink)
    );

    if (listHead_flink == 0 || listHead_flink < KERNEL_VA_MIN) {
        printf("[!] Invalid Flink: 0x%llX\n", listHead_flink);
        return false;
    }

    uint64_t current = listHead_flink;
    int count = 0;
    const int MAX_PROCS = 4096;

    while (count < MAX_PROCS) {
        uint64_t eprocess_va = current - EPROCESS_LINKS;

        uint64_t pid = m_phys.ReadVirt<uint64_t>(
            m_systemCR3, eprocess_va + EPROCESS_PID);
        uint64_t dtb = m_phys.ReadVirt<uint64_t>(
            m_systemCR3, eprocess_va + EPROCESS_DTB);
        char name[16] = {};
        m_phys.ReadVirtual(m_systemCR3,
            eprocess_va + m_imageNameOffset, name, 15);
        name[15] = '\0';

        if (_stricmp(name, imageName) == 0) {
            printf("[+] Found: %s (PID %llu, DTB 0x%llX)\n",
                   name, pid, dtb);

            out.eprocess_va = eprocess_va;
            out.eprocess_pa = m_phys.VirtToPhys(m_systemCR3, eprocess_va);
            out.dtb         = dtb;
            out.pid         = static_cast<uint32_t>(pid);
            strncpy_s(out.name, name, 15);
            return true;
        }

        uint64_t next_flink = m_phys.ReadVirt<uint64_t>(
            m_systemCR3, current);
        if (next_flink == 0 || next_flink == listHead_flink ||
            next_flink < KERNEL_VA_MIN)
            break;

        current = next_flink;
        count++;
    }

    printf("[!] '%s' not found (%d scanned)\n", imageName, count);
    return false;
}

/* ------------------------------------------------------------------ */
/*  Get Module Base via PEB Auto-Detection                             */
/*                                                                     */
/*  Finds the PEB pointer in the target EPROCESS by scanning for a     */
/*  user-mode, page-aligned pointer. Then reads PEB+0x10 to get        */
/*  ImageBaseAddress. Validates by checking for MZ header.              */
/* ------------------------------------------------------------------ */

uint64_t KernelContext::GetProcessModuleBase(const ProcessInfo& proc)
{
    if (proc.eprocess_va == 0 || m_systemCR3 == 0)
        return 0;

    /*
     * Scan the EPROCESS for a user-mode, page-aligned pointer
     * that looks like a PEB address. PEB is always in user space
     * (< 0x800000000000) and page-aligned.
     *
     * We read a range of the EPROCESS and check each 8-byte value.
     * When we find a PEB candidate, we validate it by reading
     * PEB+0x10 (ImageBaseAddress) and checking for a valid MZ header.
     */
    constexpr uint32_t SCAN_START = 0x200;
    constexpr uint32_t SCAN_END   = 0xC00;
    constexpr uint32_t SCAN_SIZE  = SCAN_END - SCAN_START;

    uint8_t buf[SCAN_SIZE] = {};
    for (uint32_t chunk = 0; chunk < SCAN_SIZE; chunk += 0x1000) {
        uint32_t chunkSize = (std::min)(static_cast<uint32_t>(0x1000),
                                        SCAN_SIZE - chunk);
        m_phys.ReadVirtual(m_systemCR3,
                           proc.eprocess_va + SCAN_START + chunk,
                           buf + chunk, chunkSize);
    }

    for (uint32_t off = 0; off + 8 <= SCAN_SIZE; off += 8) {
        uint64_t val = *reinterpret_cast<uint64_t*>(buf + off);

        /* Must be user-mode, non-zero, page-aligned */
        if (val == 0 || val >= 0x800000000000ULL || (val & 0xFFF) != 0)
            continue;

        /* PEB candidate - try reading ImageBaseAddress at PEB+0x10 */
        uint64_t imageBase = m_phys.ReadVirt<uint64_t>(proc.dtb, val + 0x10);
        if (imageBase == 0 || imageBase >= 0x800000000000ULL)
            continue;

        /* Validate: read first 2 bytes, should be MZ header */
        uint16_t mz = m_phys.ReadVirt<uint16_t>(proc.dtb, imageBase);
        if (mz != 0x5A4D)  /* 'MZ' */
            continue;

        /* Found valid PEB → ImageBase */
        printf("[+] PEB at EPROCESS+0x%X = 0x%llX, ImageBase = 0x%llX\n",
               SCAN_START + off, val, imageBase);
        fflush(stdout);
        return imageBase;
    }

    printf("[*] Could not find PEB/ImageBase for PID %u\n", proc.pid);
    fflush(stdout);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Read Process Memory                                                */
/* ------------------------------------------------------------------ */

bool KernelContext::ReadProcessMemory(const ProcessInfo& proc,
                                       uint64_t va, void* buffer,
                                       size_t size)
{
    if (proc.dtb == 0)
        return false;
    return m_phys.ReadVirtual(proc.dtb, va, buffer, size);
}

/* ------------------------------------------------------------------ */
/*  GetKernelBaseFromUserMode                                           */
/*                                                                     */
/*  Uses NtQuerySystemInformation(SystemModuleInformation) to find      */
/*  the kernel base address from user mode. No driver access needed.    */
/*  The first module returned is always ntoskrnl.exe.                   */
/* ------------------------------------------------------------------ */

uint64_t KernelContext::GetKernelBaseFromUserMode()
{
    /* Strategy 1: EnumDeviceDrivers (Psapi) - often works when NtQSI is blocked */
    LPVOID drivers[1024];
    DWORD cbNeeded;
    if (EnumDeviceDrivers(drivers, sizeof(drivers), &cbNeeded) && cbNeeded > 0) {
        int driversCount = cbNeeded / sizeof(drivers[0]);
        if (driversCount > 0) {
            uint64_t base = reinterpret_cast<uint64_t>(drivers[0]);
            printf("[+] Kernel base via EnumDeviceDrivers: 0x%llX\n", base);
            fflush(stdout);
            return base;
        }
    }

    /* Strategy 2: NtQuerySystemInformation (SystemModuleInformation) */
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll) {
        auto NtQSI = (PFN_NtQuerySystemInformation)
            GetProcAddress(hNtdll, "NtQuerySystemInformation");
        if (NtQSI) {
            ULONG needed = 0;
            NtQSI(SystemModuleInformation, nullptr, 0, &needed);
            if (needed > 0) {
                std::vector<uint8_t> buf(needed + 0x1000);
                NTSTATUS st = NtQSI(SystemModuleInformation,
                                     buf.data(),
                                     static_cast<ULONG>(buf.size()),
                                     &needed);
                if (st == 0) {
                    auto modules = reinterpret_cast<PRTL_PROCESS_MODULES>(buf.data());
                    if (modules->NumberOfModules > 0) {
                        uint64_t base = reinterpret_cast<uint64_t>(
                            modules->Modules[0].ImageBase
                        );
                        printf("[+] Kernel base via NtQSI: 0x%llX\n", base);
                        fflush(stdout);
                        return base;
                    }
                } else {
                    printf("[*] NtQSI failed (0x%08lX), EnumDeviceDrivers also failed\n", st);
                    fflush(stdout);
                }
            }
        }
    }

    printf("[!] Both EnumDeviceDrivers and NtQSI failed (error %lu)\n",
           GetLastError());
    fflush(stdout);
    return 0;
}
