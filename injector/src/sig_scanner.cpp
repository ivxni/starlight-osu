/*
 * starlight-osu :: injector/src/sig_scanner.cpp
 *
 * Pattern scanner for osu! stable.
 *
 * osu! stable is a 32-bit .NET Framework app. The CLR JIT compiles
 * methods at runtime into dynamically allocated memory -- NOT in
 * the .text section of osu!.exe. We must scan the ENTIRE process
 * virtual address space for patterns.
 *
 * Signatures sourced from community projects:
 *   - gosumemory   (l3lackShark)
 *   - osu-memory   (UnnamedOrange)
 *   - ProcessMemoryDataFinder (Piotrekol)
 *
 * Pointer chain documentation:
 *   Status:   [sig_match - 0x4]          => static addr holding game state
 *   Base:     [sig_match - 0xC]          => beatmap object pointer
 *   Rulesets: [sig_match - 0xB]          => static addr → [[addr]+0x4] = ruleset obj
 *   Mods:     [sig_match + 0x9]          => static addr holding mods bitfield
 *   PlayTime: [sig_match + 0x5]          => static addr holding audio ms
 */

#include "sig_scanner.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

/* ------------------------------------------------------------------ */
/*  Signature Definitions                                              */
/* ------------------------------------------------------------------ */

struct PatternDef {
    const char*    name;
    const uint8_t* bytes;
    const char*    mask;      /* 'x' = must match, '?' = wildcard */
    size_t         len;
    int32_t        ptrOffset; /* offset from match start to read the pointer */
};

/* Status: game state (0=menu, 2=playing, 5=songselect, 7=results)
 * gosumemory: "48 83 F8 04 73 1E", resolve: [match - 4] */
static const uint8_t PAT_STATUS[] = {
    0x48, 0x83, 0xF8, 0x04, 0x73, 0x1E
};

/* Base: beatmap info pointer
 * gosumemory: "F8 01 74 04 83 65", resolve: [match - 0xC] */
static const uint8_t PAT_BASE[] = {
    0xF8, 0x01, 0x74, 0x04, 0x83, 0x65
};

/* Rulesets: gameplay/scoring chain root
 * gosumemory: "7D 15 A1 ?? ?? ?? ?? 85 C0", resolve: [match - 0xB] */
static const uint8_t PAT_RULESETS[] = {
    0x7D, 0x15, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0
};

/* Mods: active mod bitfield
 * gosumemory: "C8 FF ?? ?? ?? ?? ?? 81 0D ?? ?? ?? ?? 00 08 00 00"
 * resolve: [match + 0x9] */
static const uint8_t PAT_MODS[] = {
    0xC8, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x0D,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00
};

/* PlayTime: audio position (ms)
 * gosumemory: "5E 5F 5D C3 A1 ?? ?? ?? ?? 89 ?? 04"
 * resolve: [match + 0x5] */
static const uint8_t PAT_PLAYTIME[] = {
    0x5E, 0x5F, 0x5D, 0xC3, 0xA1, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x00, 0x04
};

enum SigIndex {
    SIG_STATUS = 0,
    SIG_BASE,
    SIG_RULESETS,
    SIG_MODS,
    SIG_PLAYTIME,
    SIG_COUNT
};

static const PatternDef PATTERNS[SIG_COUNT] = {
    { "Status",   PAT_STATUS,   "xxxxxx",           6,  -4 },
    { "Base",     PAT_BASE,     "xxxxxx",           6,  -0xC },
    { "Rulesets", PAT_RULESETS, "xxx????xx",         9,  -0xB },
    { "Mods",     PAT_MODS,    "xx?????xx????xxxx", 17, +9 },
    { "PlayTime", PAT_PLAYTIME, "xxxxx????x?x",     12, +5 },
};


SigScanner::SigScanner(KernelContext& kernel, const ProcessInfo& proc,
                       uint64_t moduleBase)
    : m_kernel(kernel)
    , m_proc(proc)
    , m_moduleBase(moduleBase)
    , m_moduleSize(0)
    , m_statusPtr(0)
    , m_audioTimePtr(0)
    , m_rulesetPtr(0)
    , m_modsPtr(0)
    , m_playingPtr(0)
    , m_valid(false)
{
    if (m_moduleBase == 0)
        m_moduleBase = kernel.GetProcessModuleBase(proc);
}

/* ------------------------------------------------------------------ */
/*  PE Header - just reads SizeOfImage for logging                     */
/* ------------------------------------------------------------------ */

bool SigScanner::GetTextSection(uint64_t& textStart, uint64_t& textSize)
{
    if (!m_moduleBase) return false;

    uint16_t magic = Read<uint16_t>(m_moduleBase);
    if (magic != 0x5A4D) {
        printf("[!] SigScanner: Not a valid PE (magic=0x%X)\n", magic);
        return false;
    }

    uint32_t peOff = Read<uint32_t>(m_moduleBase + 0x3C);
    uint32_t sizeOfImage = Read<uint32_t>(m_moduleBase + peOff + 24 + 56);
    m_moduleSize = sizeOfImage;

    /* For .NET apps, scan the full 32-bit user-mode address space. */
    textStart = 0x10000;
    textSize  = 0x7FFF0000ULL;

    printf("[+] SigScanner: osu! image size: 0x%X\n", sizeOfImage);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Pattern Matching (single pattern in a buffer)                      */
/* ------------------------------------------------------------------ */

static int64_t MatchPattern(const uint8_t* buf, size_t bufLen,
                            const PatternDef& pat)
{
    if (bufLen < pat.len) return -1;

    for (size_t i = 0; i + pat.len <= bufLen; i++) {
        bool match = true;
        for (size_t j = 0; j < pat.len; j++) {
            if (pat.mask[j] == 'x' && buf[i + j] != pat.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) return static_cast<int64_t>(i);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  FindPattern (unchanged interface for compatibility)                 */
/* ------------------------------------------------------------------ */

uint64_t SigScanner::FindPattern(uint64_t start, uint64_t size,
                                  const uint8_t* pattern, const char* mask,
                                  size_t patLen)
{
    PatternDef pd;
    pd.bytes = pattern;
    pd.mask  = mask;
    pd.len   = patLen;

    const size_t CHUNK = 0x10000;
    const size_t OVERLAP = patLen;
    std::vector<uint8_t> buf(CHUNK + OVERLAP);

    for (uint64_t addr = start; addr < start + size; ) {
        size_t readSize = CHUNK + OVERLAP;
        if (addr + readSize > start + size)
            readSize = static_cast<size_t>((start + size) - addr);

        if (!ReadMem(addr, buf.data(), readSize)) {
            addr += CHUNK;
            continue;
        }

        bool allZero = true;
        for (size_t i = 0; i < 64 && i < readSize; i++) {
            if (buf[i] != 0) { allZero = false; break; }
        }
        if (allZero) { addr += CHUNK; continue; }

        int64_t off = MatchPattern(buf.data(), readSize, pd);
        if (off >= 0) return addr + off;

        addr += CHUNK;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Resolve: read uint32 at match + ptrOffset                         */
/* ------------------------------------------------------------------ */

uint64_t SigScanner::ResolveRelative(uint64_t instrAddr, int32_t offset_from_match)
{
    uint64_t readAddr = instrAddr + offset_from_match;
    uint32_t value = Read<uint32_t>(readAddr);
    if (!value || value < 0x10000)
        return 0;
    return static_cast<uint64_t>(value);
}

/* ------------------------------------------------------------------ */
/*  Main Scan - single pass, all patterns                              */
/* ------------------------------------------------------------------ */

bool SigScanner::ScanAll()
{
    printf("[*] SigScanner: Starting scan (module base: 0x%llX)\n", m_moduleBase);

    if (!m_moduleBase) {
        printf("[!] SigScanner: No module base\n");
        return false;
    }

    uint64_t scanStart = 0, scanSize = 0;
    if (!GetTextSection(scanStart, scanSize)) {
        printf("[!] SigScanner: Cannot determine scan range\n");
        return false;
    }

    printf("[*] SigScanner: Scanning full address space 0x%llX - 0x%llX...\n",
           scanStart, scanStart + scanSize);

    /* Single pass scan with all patterns */
    const size_t CHUNK = 0x10000;           /* 64 KB chunks */
    const size_t MAX_PAT = 32;
    const size_t OVERLAP = MAX_PAT;
    std::vector<uint8_t> buf(CHUNK + OVERLAP);

    bool found[SIG_COUNT] = {};
    uint64_t matchAddr[SIG_COUNT] = {};
    int foundCount = 0;
    uint64_t bytesScanned = 0;
    uint64_t chunksRead = 0;

    for (uint64_t addr = scanStart; addr < scanStart + scanSize && foundCount < SIG_COUNT; ) {
        size_t readSize = CHUNK + OVERLAP;
        if (addr + readSize > scanStart + scanSize)
            readSize = static_cast<size_t>((scanStart + scanSize) - addr);

        if (!ReadMem(addr, buf.data(), readSize)) {
            addr += CHUNK;
            bytesScanned += CHUNK;
            continue;
        }

        /* Quick: skip all-zero pages */
        bool allZero = true;
        for (size_t i = 0; i < 64 && i < readSize; i++) {
            if (buf[i] != 0) { allZero = false; break; }
        }
        if (allZero) {
            addr += CHUNK;
            bytesScanned += CHUNK;
            continue;
        }

        chunksRead++;

        /* Check all un-found patterns against this chunk */
        for (int p = 0; p < SIG_COUNT; p++) {
            if (found[p]) continue;

            int64_t off = MatchPattern(buf.data(), readSize, PATTERNS[p]);
            if (off >= 0) {
                matchAddr[p] = addr + off;
                found[p] = true;
                foundCount++;
            }
        }

        /* Progress every 256 chunks (~16MB) */
        if ((chunksRead & 0xFF) == 0) {
            double pct = 100.0 * bytesScanned / scanSize;
            printf("\r[*] SigScanner: %.0f%% scanned (%d/%d found)...",
                   pct, foundCount, SIG_COUNT);
            fflush(stdout);
        }

        addr += CHUNK;
        bytesScanned += CHUNK;
    }

    printf("\r[*] SigScanner: 100%% scanned (%llu chunks with data)        \n",
           (unsigned long long)chunksRead);

    /* Resolve pointers from matches */
    for (int p = 0; p < SIG_COUNT; p++) {
        if (!found[p]) {
            printf("[!] SigScanner: %-10s NOT FOUND\n", PATTERNS[p].name);
            continue;
        }

        uint64_t ptr = ResolveRelative(matchAddr[p], PATTERNS[p].ptrOffset);
        printf("[+] SigScanner: %-10s at 0x%llX -> ptr 0x%llX\n",
               PATTERNS[p].name, matchAddr[p], ptr);

        switch (p) {
            case SIG_STATUS:   m_statusPtr    = ptr; break;
            case SIG_BASE:     m_playingPtr   = ptr; break;
            case SIG_RULESETS: m_rulesetPtr   = ptr; break;
            case SIG_MODS:     m_modsPtr      = ptr; break;
            case SIG_PLAYTIME: m_audioTimePtr = ptr; break;
        }
    }

    printf("[*] SigScanner: %d/%d signatures resolved\n", foundCount, SIG_COUNT);

    /* We need at least PlayTime and Rulesets for relax to work */
    m_valid = (m_audioTimePtr != 0) && (m_rulesetPtr != 0);

    if (!m_valid) {
        printf("[!] SigScanner: Critical signatures missing (need PlayTime + Rulesets).\n");
        printf("[!] osu! may have updated. Signatures need refreshing.\n");
    }

    return m_valid;
}
