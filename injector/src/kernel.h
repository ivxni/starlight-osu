#pragma once
/*
 * starlight-sys :: injector/src/kernel.h
 *
 * Kernel navigation utilities - operates entirely through physical
 * memory reads via the vulnerable driver. No kernel driver needed.
 *
 * SAFETY:
 *   1. Reads RAM ranges from Windows registry
 *   2. Installs PA safety filter on PhysicalMemory (rejects MMIO reads)
 *   3. Tries CR3 brute-force first (reads only first 16MB - always safe)
 *   4. Falls back to limited EPROCESS scan (first 2GB, PA-filtered)
 *   5. Rate-limits physical reads to reduce driver stress
 */

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "physmem.h"    /* includes MemRange */
#include "../../shared/driver_backend.h"

/* Process info structure returned by FindProcess */
struct ProcessInfo {
    uint64_t eprocess_va;
    uint64_t eprocess_pa;
    uint64_t dtb;
    uint32_t pid;
    char     name[16];
};

class KernelContext {
public:
    explicit KernelContext(PhysicalMemory& phys);
    ~KernelContext();

    /*
     * PreloadSafetyFilter - load RAM ranges from registry and install
     * PA safety filter on PhysicalMemory. Call this BEFORE loading the
     * driver or doing any physical reads. Does not require the driver.
     *
     * After this, every ReadPhysical call is checked against safe ranges.
     */
    bool PreloadSafetyFilter();

    /*
     * Initialize - find System process and establish kernel context.
     *
     * Parameters:
     *   skipMSR - if true, skip reading MSR LSTAR (safer on some systems)
     *
     * Steps:
     *   1. (Optional) Load RAM ranges if not already loaded by PreloadSafetyFilter
     *   2. (Optional) Read MSR LSTAR for ntoskrnl VA estimate
     *   3. Try CR3 brute-force (fast, first 16MB only)
     *   3.5 If CR3 found: resolve PsInitialSystemProcess export (~20 reads)
     *   4. Fallback: scan limited RAM for System EPROCESS (PID=4)
     *   5. Validate CR3 if LSTAR was obtained
     */
    bool Initialize(bool skipMSR = false);

    bool FindProcess(const char* imageName, ProcessInfo& out);
    uint64_t GetProcessModuleBase(const ProcessInfo& proc);
    bool ReadProcessMemory(const ProcessInfo& proc, uint64_t va,
                           void* buffer, size_t size);

    template<typename T>
    T ReadProcess(const ProcessInfo& proc, uint64_t va) {
        T val{};
        ReadProcessMemory(proc, va, &val, sizeof(T));
        return val;
    }

    uint64_t GetSystemCR3()        const { return m_systemCR3; }
    uint64_t GetNtoskrnlBase()     const { return m_ntoskrnlVA; }
    uint64_t GetSystemEprocessVA() const { return m_systemEprocessVA; }
    uint32_t GetImageNameOffset()  const { return m_imageNameOffset; }
    bool     IsInitialized()       const { return m_systemCR3 != 0; }

private:
    PhysicalMemory& m_phys;

    uint64_t m_systemCR3;
    uint64_t m_systemEprocessPA;
    uint64_t m_systemEprocessVA;
    uint64_t m_ntoskrnlVA;
    uint32_t m_imageNameOffset;  /* auto-detected ImageFileName offset */

    std::vector<MemRange> m_ramRanges;

    /*
     * Maximum physical address to scan for EPROCESS (fallback only).
     * System EPROCESS (PID=4) is almost always allocated in the
     * first few hundred MB. Reduced from 2GB to 512MB because the
     * primary path now uses PsInitialSystemProcess export resolution.
     */
    static constexpr uint64_t MAX_EPROCESS_SCAN_PA = 0x100000000ULL; /* 4 GB */

    /* Auto-detect ImageFileName offset within EPROCESS */
    bool DetectImageNameOffset();

    /* Query registry for physical RAM ranges */
    bool LoadMemoryMap();

    /*
     * CR3 brute-force: try page-aligned addresses in first 16MB.
     * Uses LSTAR to validate - translates LSTAR VA through each
     * candidate CR3 and checks if the result is plausible code.
     * Returns true if m_systemCR3 was found.
     */
    bool FindSystemCR3(uint64_t lstarVA);

    /*
     * Find System EPROCESS via PsInitialSystemProcess export.
     *
     * Loads ntoskrnl.exe into user-mode (PE mapping only, no execution)
     * via LoadLibraryExA + DONT_RESOLVE_DLL_REFERENCES, finds the
     * PsInitialSystemProcess export, calculates the kernel VA, and
     * reads the EPROCESS pointer through the page tables.
     *
     * Requires: m_systemCR3 != 0 and m_ntoskrnlVA != 0.
     * Total physical reads: ~20 (a few VirtToPhys walks + value reads).
     *
     * This is the PRIMARY method. If it succeeds, the heavy
     * ScanForSystemProcess() is skipped entirely.
     */
    bool FindSystemEprocessViaExport();

    /* Scan within safe RAM ranges (limited to MAX_EPROCESS_SCAN_PA) */
    bool ScanForSystemProcess();

    /* Validate CR3 by translating a known VA */
    bool ValidateCR3(uint64_t cr3, uint64_t knownVA);

    /*
     * Get kernel base via NtQuerySystemInformation(SystemModuleInformation).
     * This works from user mode without any driver access.
     * Used as alternative to MSR LSTAR when driver doesn't support MSR.
     */
    uint64_t GetKernelBaseFromUserMode();
};
