#pragma once
/*
 * starlight-osu :: injector/src/osu_reader.h
 *
 * Background thread that reads osu! stable game state via kernel
 * memory reads into a shared snapshot for the overlay and assists.
 *
 * osu! stable is a 32-bit .NET Framework application. Memory structures
 * are accessed through pointer chains resolved from signature-scanned
 * base addresses.
 *
 * Coordinate system: osu! playfield is 512x384 "osu! pixels".
 */

#include <cstdint>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <string>
#include "kernel.h"
#include "physmem.h"
#include "sig_scanner.h"

/* ------------------------------------------------------------------ */
/*  osu! Offset Chains                                                 */
/*                                                                     */
/*  These are pointer dereference chains from sig-scanned base addrs.  */
/*  osu! stable is 32-bit, so all pointers are uint32.                 */
/*                                                                     */
/*  Notation: Base -> [+0x4] -> [+0x60] means:                         */
/*    ptr1 = Read32(Base)                                              */
/*    ptr2 = Read32(ptr1 + 0x4)                                        */
/*    val  = Read32(ptr2 + 0x60)                                       */
/* ------------------------------------------------------------------ */

namespace OsuOffsets {
    /* Status enum values */
    constexpr int32_t STATUS_MENU       = 0;
    constexpr int32_t STATUS_EDITING    = 1;
    constexpr int32_t STATUS_PLAYING    = 2;
    constexpr int32_t STATUS_EXIT       = 3;
    constexpr int32_t STATUS_SONGSELECT = 5;
    constexpr int32_t STATUS_MULTIPLAYER = 11;
    constexpr int32_t STATUS_RANKING    = 7;

    /* Mod bitflags */
    constexpr int32_t MOD_NOMOD   = 0;
    constexpr int32_t MOD_NF      = 1;
    constexpr int32_t MOD_EZ      = 2;
    constexpr int32_t MOD_TD      = 4;
    constexpr int32_t MOD_HD      = 8;
    constexpr int32_t MOD_HR      = 16;
    constexpr int32_t MOD_SD      = 32;
    constexpr int32_t MOD_DT      = 64;
    constexpr int32_t MOD_RX      = 128;
    constexpr int32_t MOD_HT      = 256;
    constexpr int32_t MOD_NC      = 512;
    constexpr int32_t MOD_FL      = 1024;
    constexpr int32_t MOD_AUTO    = 2048;
    constexpr int32_t MOD_SO      = 4096;
    constexpr int32_t MOD_AP      = 8192;  /* Autopilot */
    constexpr int32_t MOD_PF      = 16384;

    /* Ruleset pointer chain offsets */
    constexpr uint32_t RULESET_BASE_OFFSETS[]   = { 0x68 };
    constexpr uint32_t BEATMAP_ADDR_OFFSET      = 0x04;

    /* Hit object list (from ruleset base) */
    constexpr uint32_t HIT_OBJECTS_OFFSET       = 0x48;
    constexpr uint32_t HIT_OBJECTS_COUNT_OFFSET  = 0x0C; /* TArray.Count in .NET List */
    constexpr uint32_t HIT_OBJECTS_ITEMS_OFFSET  = 0x04; /* TArray._items (array ptr) */

    /* Individual hit object fields (from object pointer) */
    constexpr uint32_t HO_X_OFFSET         = 0x38;
    constexpr uint32_t HO_Y_OFFSET         = 0x3C;
    constexpr uint32_t HO_TIME_OFFSET      = 0x10;
    constexpr uint32_t HO_TYPE_OFFSET      = 0x18;
    constexpr uint32_t HO_END_TIME_OFFSET  = 0x1C; /* sliders/spinners */

    /* Audio time: direct pointer -> int32 */
    constexpr uint32_t AUDIO_TIME_OFFSET   = 0x00;

    /* Beatmap difficulty (from beatmap object) */
    constexpr uint32_t BEATMAP_AR_OFFSET   = 0x2C;
    constexpr uint32_t BEATMAP_CS_OFFSET   = 0x30;
    constexpr uint32_t BEATMAP_HP_OFFSET   = 0x34;
    constexpr uint32_t BEATMAP_OD_OFFSET   = 0x38;

    /* Player (from ruleset base) */
    constexpr uint32_t PLAYER_HP_OFFSET    = 0x40;
    constexpr uint32_t PLAYER_COMBO_OFFSET = 0x48;
    constexpr uint32_t PLAYER_SCORE_OFFSET = 0x78;
}

/* ------------------------------------------------------------------ */
/*  Data Structures                                                    */
/* ------------------------------------------------------------------ */

/* Hit object types */
enum class HitObjectType : uint8_t {
    Circle  = 1,
    Slider  = 2,
    Spinner = 8,
};

struct HitObject {
    float   x = 0, y = 0;          /* osu! pixels (0-512, 0-384) */
    int32_t time = 0;              /* start time in ms */
    int32_t endTime = 0;           /* end time for sliders/spinners */
    int32_t type = 0;              /* raw type bitfield */
    bool    isCircle = false;
    bool    isSlider = false;
    bool    isSpinner = false;
    bool    isValid = false;
};

struct BeatmapInfo {
    float AR = 0, CS = 0, OD = 0, HP = 0;
    bool  valid = false;
};

struct OsuSnapshot {
    /* Game state */
    int32_t gameState = 0;         /* 0=menu, 2=playing, 7=results */
    bool    isPlaying = false;

    /* Audio */
    int32_t audioTime = 0;         /* current audio position (ms) */

    /* Mods */
    int32_t mods = 0;              /* bitfield */

    /* Beatmap */
    BeatmapInfo beatmap;

    /* Hit objects (sorted by time) */
    HitObject hitObjects[2048];
    uint32_t  hitObjectCount = 0;

    /* Current hit object index (first not yet hit) */
    uint32_t  currentHitIndex = 0;

    /* Player state */
    float     playerHP = 0;
    int32_t   combo = 0;
    int32_t   score = 0;

    /* Validity */
    bool      valid = false;

    /* Computed values */
    float     hitWindow300 = 0;    /* ms: perfect timing window */
    float     hitWindow100 = 0;    /* ms: good timing window */
    float     hitWindow50 = 0;     /* ms: ok timing window */
    float     circleRadius = 0;    /* osu! pixels: circle size */
};

/* ------------------------------------------------------------------ */
/*  Game Reader Thread                                                 */
/* ------------------------------------------------------------------ */

class OsuReader {
public:
    OsuReader(KernelContext& kernel, PhysicalMemory& phys,
              const ProcessInfo& proc, SigScanner& sigs);
    ~OsuReader();

    void Start();
    void Stop();

    /* Copy the latest game snapshot (thread-safe) */
    OsuSnapshot GetSnapshot();

    /* Debug info */
    struct DebugInfo {
        std::atomic<uint32_t> frameCount{0};
        std::atomic<int32_t>  lastGameState{-1};
        std::atomic<int32_t>  lastAudioTime{0};
        std::atomic<uint32_t> hitObjectsFound{0};
        std::atomic<bool>     sigsValid{false};
    } debug;

private:
    void ReadLoop();

    /* Read individual game state components */
    bool ReadGameState();
    bool ReadAudioTime();
    bool ReadMods();
    bool ReadBeatmapInfo();
    bool ReadHitObjects();
    bool ReadPlayerState();

    /* Compute derived values from beatmap difficulty */
    void ComputeHitWindows();

    /* Find current hit object index based on audio time */
    void UpdateCurrentHitIndex();

    template<typename T>
    T Read(uint64_t va) {
        return m_kernel.ReadProcess<T>(m_proc, va);
    }

    bool ReadMem(uint64_t va, void* buf, size_t sz) {
        return m_kernel.ReadProcessMemory(m_proc, va, buf, sz);
    }

    /* Read 32-bit pointer (osu! is 32-bit) */
    uint64_t ReadPtr32(uint64_t va) {
        uint32_t v = Read<uint32_t>(va);
        return static_cast<uint64_t>(v);
    }

    /* Follow a chain of 32-bit pointer dereferences */
    uint64_t FollowPtrChain(uint64_t base,
                            const uint32_t* offsets, size_t count);

    /* Read a .NET Framework string from a pointer field in memory */
    std::string ReadDotNetString(uint64_t stringPtrAddr);

    /* Parse hit objects from a .osu file on disk */
    bool ParseOsuFile(const std::string& path);

    KernelContext&  m_kernel;
    PhysicalMemory& m_phys;
    ProcessInfo     m_proc;
    SigScanner&     m_sigs;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};

    std::mutex    m_mutex;
    OsuSnapshot   m_snapshot;

    /* Cached: hit objects are only re-read on map change */
    int32_t       m_lastBeatmapTime = -1;
    bool          m_hitObjectsCached = false;
};
