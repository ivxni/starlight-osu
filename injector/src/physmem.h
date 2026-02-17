#pragma once
/*
 * starlight-sys :: injector/src/physmem.h
 *
 * READ-ONLY physical memory access via pluggable driver backends.
 * Supports: LnvMSRIO.sys (CVE-2025-8061), ThrottleStop.sys (CVE-2025-7771)
 *
 * SAFETY:
 *   - PA range filter: rejects reads outside known RAM ranges BEFORE
 *     the IOCTL reaches the driver. Drivers may not check the return
 *     value of MmMapIoSpace; the filter prevents BSOD.
 *   - RAII sessions: handle is opened/closed per session to minimize
 *     detection window for handle scanners (Vanguard).
 *   - No write IOCTLs exist in this class.
 */

#include <Windows.h>
#include <cstdint>
#include <vector>
#include "../../shared/ioctl_codes.h"
#include "../../shared/driver_backend.h"

/* Physical memory range descriptor */
struct MemRange {
    uint64_t start;
    uint64_t end;   /* exclusive */
};

class PhysicalMemory {
public:
    /*
     * Constructor takes a driver backend pointer (not owned).
     * The backend defines IOCTL codes, device path, and chunk sizes.
     * If nullptr, defaults to LnvMSRIO backend.
     */
    explicit PhysicalMemory(IDriverBackend* backend = nullptr);
    ~PhysicalMemory();

    /* ---- PA safety filter ---- */
    /*
     * SetSafeRanges - provide known-good physical RAM ranges.
     * When set, ReadPhysical rejects ANY address not fully within
     * one of these ranges (zero-fills buffer, returns false).
     * This prevents the driver from calling MmMapIoSpace on
     * MMIO/reserved regions → eliminates BSOD risk.
     *
     * Ranges typically come from the Windows registry:
     *   HKLM\HARDWARE\RESOURCEMAP\System Resources\Physical Memory
     */
    void SetSafeRanges(const std::vector<MemRange>& ranges);
    void ClearSafeRanges();
    bool IsAddressSafe(uint64_t pa, size_t size) const;

    /* ---- Explicit handle management ---- */
    bool Open();        /* opens \\.\WinMsrDev */
    void Close();       /* closes the handle immediately */
    bool IsOpen() const { return m_hDevice != INVALID_HANDLE_VALUE; }

    /*
     * RAII Session - minimizes handle exposure time.
     */
    class Session {
    public:
        explicit Session(PhysicalMemory& pm)
            : m_pm(pm), m_ok(false), m_wasAlreadyOpen(false)
        {
            if (pm.IsOpen()) {
                m_wasAlreadyOpen = true;
                m_ok = true;
            } else {
                m_ok = pm.Open();
            }
        }

        ~Session() {
            if (m_ok && !m_wasAlreadyOpen)
                m_pm.Close();
        }

        bool IsValid() const { return m_ok; }
        operator bool() const { return m_ok; }

    private:
        PhysicalMemory& m_pm;
        bool m_ok;
        bool m_wasAlreadyOpen;
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
    };

    /* ---- Physical Memory READ ---- */
    bool ReadPhysical(uint64_t physAddr, void* buffer, size_t size);

    /* ---- MSR READ ---- */
    uint64_t ReadMSR(uint32_t reg);

    /* ---- Page Table Walk (VA -> PA) ---- */
    uint64_t VirtToPhys(uint64_t cr3, uint64_t virtualAddr);

    /* ---- Virtual Memory READ (page-boundary aware) ---- */
    bool ReadVirtual(uint64_t cr3, uint64_t va, void* buffer, size_t size);

    /* ---- Template helpers (read-only) ---- */
    template<typename T>
    T ReadPhys(uint64_t pa) {
        T val{};
        ReadPhysical(pa, &val, sizeof(T));
        return val;
    }

    template<typename T>
    T ReadVirt(uint64_t cr3, uint64_t va) {
        T val{};
        ReadVirtual(cr3, va, &val, sizeof(T));
        return val;
    }

    /* ---- I/O Port Access ---- */
    bool SupportsPortIO() const { return m_backend->SupportsPortIO(); }

    bool WriteIoPortByte(uint16_t port, uint8_t val) {
        if (m_hDevice == INVALID_HANDLE_VALUE) return false;
        return m_backend->WriteIoPortByte(m_hDevice, port, val);
    }

    uint8_t ReadIoPortByte(uint16_t port) {
        if (m_hDevice == INVALID_HANDLE_VALUE) return 0;
        return m_backend->ReadIoPortByte(m_hDevice, port);
    }

    /* ---- Backend info ---- */
    IDriverBackend* GetBackend() const { return m_backend; }
    HANDLE GetHandle() const { return m_hDevice; }

private:
    HANDLE m_hDevice;
    IDriverBackend* m_backend;          /* not owned */
    LnvMSRIOBackend m_defaultBackend;   /* fallback if no backend given */
    std::vector<MemRange> m_safeRanges;
    bool m_hasSafeRanges;
};
