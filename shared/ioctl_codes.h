#pragma once
/*
 * starlight-sys :: shared/ioctl_codes.h
 *
 * READ-ONLY IOCTL codes and structures for LnvMSRIO.sys (CVE-2025-8061)
 * Lenovo Process Management Driver v3.1.0.35
 *
 * Reference: Quarkslab - "BYOVD to the next level (part 1)"
 * https://blog.quarkslab.com/exploiting-lenovo-driver-cve-2025-8061.html
 *
 * Device: \\.\WinMsrDev (no access controls)
 *
 * SECURITY NOTE:
 *   Only READ IOCTLs are defined here. Write IOCTLs are intentionally
 *   excluded to prevent accidental or detectable memory writes.
 *   Writing to game/kernel memory = instant detection by Vanguard.
 */

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  IOCTL Control Codes - LnvMSRIO.sys (READ-ONLY)                    */
/* ------------------------------------------------------------------ */

#define LNVMSRIO_IOCTL_READ_PHYS     0x9c406104   /* Physical memory read  */
#define LNVMSRIO_IOCTL_READ_MSR      0x9c402084   /* MSR register read     */

/* Write IOCTLs intentionally NOT defined:
 *   0x9c40a108 - Physical memory write (DANGEROUS - do NOT use)
 *   0x9c402088 - MSR register write    (DANGEROUS - do NOT use)
 */

/* ------------------------------------------------------------------ */
/*  Device Name                                                        */
/* ------------------------------------------------------------------ */

#define LNVMSRIO_DEVICE_NAME_A       "\\\\.\\WinMsrDev"
#define LNVMSRIO_DEVICE_NAME_W       L"\\\\.\\WinMsrDev"
/*
 * Default service name for SCM registration.
 * Using a generic hardware-service name instead of the real driver name
 * ("LnvMSRIO") to avoid pattern-matching by anti-cheat service scanners.
 * The driver creates \\.\WinMsrDev regardless of the service name.
 * Override with --service <name> at runtime.
 */
#define LNVMSRIO_SERVICE_NAME_DEFAULT  L"HwServiceHost"

/* ------------------------------------------------------------------ */
/*  IOCTL Structures (packed, must match driver expectations)           */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

/*
 * Physical Memory Read Input
 * Total: 16 bytes (0x10) - driver validates exact size
 *
 * Flow in the driver:
 *   1. MmMapIoSpace(PhysicalAddress, Size, MmNonCached)
 *   2. memcpy(OutputBuffer, MappedRegion, Size)
 *   3. MmUnmapIoSpace(MappedRegion, Size)
 */
typedef struct _LNVMSRIO_PHYS_READ {
    uint64_t PhysicalAddress;   /* [0x00] Physical address to read      */
    uint32_t OperationType;     /* [0x08] Copy granularity (1=byte)     */
    uint32_t Size;              /* [0x0C] Number of bytes to read       */
} LNVMSRIO_PHYS_READ;

static_assert(sizeof(LNVMSRIO_PHYS_READ) == 0x10,
    "LNVMSRIO_PHYS_READ must be exactly 16 bytes");

/*
 * MSR Register Read Input
 * Returns uint64_t value in OutputBuffer
 */
typedef struct _LNVMSRIO_MSR_READ {
    uint32_t Register;          /* MSR register identifier              */
} LNVMSRIO_MSR_READ;

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/*  Windows Kernel Offsets                                              */
/*  Verify with WinDbg: dt nt!_EPROCESS                                */
/*                                                                     */
/*  NOTE: ImageFileName is auto-detected at runtime by scanning the    */
/*  System EPROCESS for "System\0".  The hardcoded value below is      */
/*  only a fallback.                                                   */
/*                                                                     */
/*  Known ImageFileName offsets:                                        */
/*    Build 22000-26100 (Win11 21H2-24H2): 0x5A8                      */
/*    Build 26200       (Win11 24H2.x):    0x338                       */
/* ------------------------------------------------------------------ */

#define EPROCESS_DTB                 0x28    /* DirectoryTableBase      */
#define EPROCESS_PID                 0x1D0   /* UniqueProcessId         */
#define EPROCESS_LINKS               0x1D8   /* ActiveProcessLinks      */
#define EPROCESS_TOKEN               0x248   /* Token                   */
#define EPROCESS_IMAGENAME           0x338   /* ImageFileName (15 ch) - Build 26200 */

/* MSR registers */
#define MSR_LSTAR                    0xC0000082  /* syscall entry addr  */

/* Page table constants */
#define PTE_PRESENT_BIT              (1ULL << 0)
#define PTE_LARGE_PAGE_BIT           (1ULL << 7)
#define PTE_FRAME_MASK               0x000FFFFFFFFFF000ULL

/* Kernel VA range */
#define KERNEL_VA_MIN                0xFFFF800000000000ULL

#ifdef __cplusplus
}
#endif
