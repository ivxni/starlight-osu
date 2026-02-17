#pragma once
/*
 * starlight-osu :: injector/src/sig_scanner.h
 *
 * Pattern/signature scanner for osu! stable (.NET/C# process).
 * Scans the process code section via kernel physical memory reads
 * to find base pointers that change with game updates.
 *
 * Each signature is a byte pattern with wildcards (0x00 in mask).
 * The scanner returns the address where the pattern was found,
 * from which we extract pointer offsets to game data.
 */

#include <cstdint>
#include <vector>
#include <string>
#include "kernel.h"
#include "physmem.h"

/* A single signature definition */
struct SigPattern {
    const char*  name;
    const uint8_t* pattern;
    const char*    mask;      /* 'x' = match, '?' = wildcard */
    size_t         length;
    int32_t        offset;    /* offset from match to extract pointer */
    bool           relative;  /* true = read int32 at offset (RIP-relative) */
};

/* Result of a signature scan */
struct SigResult {
    std::string name;
    uint64_t    address;      /* address where pattern matched */
    uint64_t    resolved;     /* final resolved pointer/value */
    bool        found;
};

class SigScanner {
public:
    SigScanner(KernelContext& kernel, const ProcessInfo& proc,
               uint64_t moduleBase = 0);

    /*
     * ScanAll - scan the osu! module for all known signatures.
     * Returns true if at least the critical signatures were found.
     *
     * Reads the PE header to find .text section bounds, then
     * scans in chunks via kernel memory reads.
     */
    bool ScanAll();

    /* Accessors for resolved addresses */
    uint64_t GetStatusPtr()     const { return m_statusPtr; }
    uint64_t GetAudioTimePtr()  const { return m_audioTimePtr; }
    uint64_t GetRulesetPtr()    const { return m_rulesetPtr; }
    uint64_t GetModsPtr()       const { return m_modsPtr; }
    uint64_t GetPlayingPtr()    const { return m_playingPtr; }

    bool IsValid() const { return m_valid; }

private:
    KernelContext& m_kernel;
    ProcessInfo    m_proc;
    uint64_t       m_moduleBase;
    uint64_t       m_moduleSize;

    /* Resolved addresses */
    uint64_t m_statusPtr;
    uint64_t m_audioTimePtr;
    uint64_t m_rulesetPtr;
    uint64_t m_modsPtr;
    uint64_t m_playingPtr;
    bool     m_valid;

    /* Read PE header to get .text section info */
    bool GetTextSection(uint64_t& textStart, uint64_t& textSize);

    /* Scan a memory region for a pattern */
    uint64_t FindPattern(uint64_t start, uint64_t size,
                         const uint8_t* pattern, const char* mask,
                         size_t patLen);

    /* Read a relative int32 offset and resolve to absolute address */
    uint64_t ResolveRelative(uint64_t instrAddr, int32_t offset_from_match);

    /* Helper: read process memory */
    template<typename T>
    T Read(uint64_t va) {
        return m_kernel.ReadProcess<T>(m_proc, va);
    }

    bool ReadMem(uint64_t va, void* buf, size_t sz) {
        return m_kernel.ReadProcessMemory(m_proc, va, buf, sz);
    }
};
