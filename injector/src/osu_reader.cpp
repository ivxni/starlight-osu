/*
 * starlight-osu :: injector/src/osu_reader.cpp
 *
 * Background thread that polls osu! stable game state via kernel
 * memory reads. Follows pointer chains from signature-scanned bases.
 *
 * Hit objects are loaded from the .osu beatmap FILE on disk (stable
 * and version-independent) rather than traversing fragile .NET object
 * graphs in memory. We read the beatmap path from the Beatmap object
 * in memory (gosumemory-verified offsets), then parse the file.
 *
 * osu! stable is 32-bit. All pointers are 4 bytes.
 */

#include "osu_reader.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>

OsuReader::OsuReader(KernelContext& kernel, PhysicalMemory& phys,
                     const ProcessInfo& proc, SigScanner& sigs)
    : m_kernel(kernel), m_phys(phys), m_proc(proc), m_sigs(sigs)
{
}

OsuReader::~OsuReader()
{
    Stop();
}

void OsuReader::Start()
{
    m_running = true;
    m_thread = std::thread(&OsuReader::ReadLoop, this);
}

void OsuReader::Stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

OsuSnapshot OsuReader::GetSnapshot()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

/* ------------------------------------------------------------------ */
/*  Pointer Chain Helper                                               */
/* ------------------------------------------------------------------ */

uint64_t OsuReader::FollowPtrChain(uint64_t base,
                                    const uint32_t* offsets, size_t count)
{
    uint64_t addr = base;
    for (size_t i = 0; i < count; i++) {
        uint32_t ptr = Read<uint32_t>(addr + offsets[i]);
        if (ptr == 0 || ptr < 0x10000)
            return 0;
        addr = static_cast<uint64_t>(ptr);
    }
    return addr;
}

/* ------------------------------------------------------------------ */
/*  .NET String Reader                                                 */
/* ------------------------------------------------------------------ */

std::string OsuReader::ReadDotNetString(uint64_t stringPtrAddr)
{
    /* Read the .NET String object pointer */
    uint32_t strObj = Read<uint32_t>(stringPtrAddr);
    if (!strObj || strObj < 0x10000) return {};

    /* .NET Framework 32-bit String layout:
     *   +0x04: int m_stringLength (char count)
     *   +0x08: first char (UTF-16LE)
     */
    uint64_t obj = static_cast<uint64_t>(strObj);
    int32_t len = Read<int32_t>(obj + 0x04);
    if (len <= 0 || len > 1024) return {};

    /* Read UTF-16LE chars */
    std::vector<uint16_t> wchars(len);
    if (!ReadMem(obj + 0x08, wchars.data(), len * sizeof(uint16_t)))
        return {};

    /* Convert UTF-16LE to narrow string (ASCII-safe for file paths) */
    std::string result;
    result.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        uint16_t c = wchars[i];
        if (c == 0) break;
        result += static_cast<char>(c < 128 ? c : '?');
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  .osu File Parser                                                   */
/* ------------------------------------------------------------------ */

bool OsuReader::ParseOsuFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        printf("[!] OsuReader: Cannot open '%s'\n", path.c_str());
        fflush(stdout);
        return false;
    }

    /* ---- Timing data collected from [Difficulty] and [TimingPoints] ---- */
    double sliderMultiplier = 1.4;

    struct TP {
        int    time;
        double beatLength;
        bool   uninherited;
    };
    std::vector<TP> timingPoints;

    /* ---- Section state machine ---- */
    enum class Sec { None, Difficulty, TimingPoints, HitObjects };
    Sec section = Sec::None;

    m_snapshot.hitObjectCount = 0;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        /* Section headers */
        if (line[0] == '[') {
            if (line == "[Difficulty]")     section = Sec::Difficulty;
            else if (line == "[TimingPoints]") section = Sec::TimingPoints;
            else if (line == "[HitObjects]")   section = Sec::HitObjects;
            else section = Sec::None;
            continue;
        }

        switch (section) {

        /* ---- [Difficulty] ---- */
        case Sec::Difficulty: {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                while (!val.empty() && val[0] == ' ') val.erase(0, 1);
                if (key == "SliderMultiplier")
                    sliderMultiplier = atof(val.c_str());
            }
            break;
        }

        /* ---- [TimingPoints] ---- */
        case Sec::TimingPoints: {
            /* Format: time,beatLength,meter,sampleSet,sampleIndex,volume,uninherited,effects */
            TP tp = {};
            int field = 0;
            const char* p = line.c_str();
            while (*p && field < 8) {
                if (field == 0) tp.time = atoi(p);
                else if (field == 1) tp.beatLength = atof(p);
                else if (field == 6) tp.uninherited = (atoi(p) == 1);
                field++;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            if (field >= 2) {
                if (field < 7)
                    tp.uninherited = (tp.beatLength > 0);
                timingPoints.push_back(tp);
            }
            break;
        }

        /* ---- [HitObjects] ---- */
        case Sec::HitObjects: {
            if (m_snapshot.hitObjectCount >= 2048) break;

            int x = 0, y = 0, time = 0, type = 0;
            int endTime = 0;

            int field = 0;
            const char* p = line.c_str();
            while (*p) {
                int val = atoi(p);
                switch (field) {
                    case 0: x = val; break;
                    case 1: y = val; break;
                    case 2: time = val; break;
                    case 3: type = val; break;
                }
                field++;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
                if (field > 4) break;
            }
            if (field < 4) break;

            bool isCircle  = (type & 1) != 0;
            bool isSlider  = (type & 2) != 0;
            bool isSpinner = (type & 8) != 0;

            if (isSpinner) {
                const char* q = line.c_str();
                int f2 = 0;
                while (*q && f2 < 5) {
                    while (*q && *q != ',') q++;
                    if (*q == ',') { q++; f2++; }
                }
                if (f2 == 5) endTime = atoi(q);
            } else if (isSlider) {
                /* Parse slides (field 6) and pixelLength (field 7) */
                const char* q = line.c_str();
                int f2 = 0;
                while (*q && f2 < 6) {
                    while (*q && *q != ',') q++;
                    if (*q == ',') { q++; f2++; }
                }
                int    slides = 1;
                double pixelLength = 0;
                if (f2 == 6) {
                    slides = atoi(q);
                    if (slides < 1) slides = 1;
                    while (*q && *q != ',') q++;
                    if (*q == ',') { q++; pixelLength = atof(q); }
                }

                if (pixelLength > 0 && !timingPoints.empty()) {
                    /* Find active uninherited BPM and inherited SV at this time */
                    double baseBeat = 500.0;
                    double speedMul = 1.0;
                    for (const auto& tp : timingPoints) {
                        if (tp.time > time) break;
                        if (tp.uninherited) {
                            baseBeat = tp.beatLength;
                            speedMul = 1.0;
                        } else {
                            speedMul = (tp.beatLength < 0)
                                ? -100.0 / tp.beatLength : 1.0;
                        }
                    }
                    if (baseBeat <= 0) baseBeat = 500.0;
                    if (speedMul <= 0) speedMul = 1.0;

                    double duration = pixelLength * slides * baseBeat
                                    / (sliderMultiplier * 100.0 * speedMul);
                    endTime = time + static_cast<int>(duration);
                    if (endTime < time + 30) endTime = time + 30;
                    if (endTime > time + 30000) endTime = time + 5000;
                } else {
                    endTime = time + 200;
                }
            }

            HitObject& ho = m_snapshot.hitObjects[m_snapshot.hitObjectCount];
            ho.x = static_cast<float>(x);
            ho.y = static_cast<float>(y);
            ho.time = time;
            ho.endTime = endTime > time ? endTime : time;
            ho.type = type;
            ho.isCircle = isCircle;
            ho.isSlider = isSlider;
            ho.isSpinner = isSpinner;
            ho.isValid = true;
            if (isSpinner) { ho.x = 256; ho.y = 192; }
            m_snapshot.hitObjectCount++;
            break;
        }

        default: break;
        }
    }

    if (m_snapshot.hitObjectCount > 0 && !timingPoints.empty()) {
        printf("[*] OsuReader: parsed %zu timing points, SM=%.2f\n",
               timingPoints.size(), sliderMultiplier);
        fflush(stdout);
    }

    return m_snapshot.hitObjectCount > 0;
}

/* ------------------------------------------------------------------ */
/*  Main Read Loop                                                     */
/* ------------------------------------------------------------------ */

void OsuReader::ReadLoop()
{
    printf("[+] OsuReader: thread started\n");

    debug.sigsValid.store(m_sigs.IsValid());

    bool loggedPlaying = false;
    std::string lastBeatmapPath;

    while (m_running) {
        PhysicalMemory::Session session(m_phys);
        if (!session) {
            Sleep(100);
            continue;
        }

        OsuSnapshot newSnap{};

        /* Read game state first - determines what else to read */
        if (ReadGameState()) {
            newSnap.gameState = m_snapshot.gameState;
            newSnap.isPlaying = (newSnap.gameState == OsuOffsets::STATUS_PLAYING);
        }

        /* Read audio time (always, even in menus for preview) */
        ReadAudioTime();
        newSnap.audioTime = m_snapshot.audioTime;

        /* Read mods */
        ReadMods();
        newSnap.mods = m_snapshot.mods;

        /* When playing: read beatmap info, hit objects from file, player state */
        if (newSnap.isPlaying || newSnap.gameState == OsuOffsets::STATUS_PLAYING) {
            if (!loggedPlaying) {
                printf("[*] OsuReader: Playing detected (state=%d, audio=%d)\n",
                       newSnap.gameState, newSnap.audioTime);
                fflush(stdout);
                loggedPlaying = true;
            }
            ReadBeatmapInfo();
            ReadHitObjects();
            ReadPlayerState();
        } else {
            if (loggedPlaying) {
                printf("[*] OsuReader: Stopped playing (state=%d)\n", newSnap.gameState);
                fflush(stdout);
                loggedPlaying = false;
            }
            /* Reset cached hit objects when not playing */
            m_hitObjectsCached = false;
            m_lastBeatmapTime = -1;
        }

        /* Build final snapshot under lock */
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            m_snapshot.gameState = newSnap.gameState;
            m_snapshot.isPlaying = newSnap.isPlaying;
            m_snapshot.audioTime = newSnap.audioTime;
            m_snapshot.mods = newSnap.mods;

            /* Compute hit windows from OD */
            ComputeHitWindows();

            /* Update current hit index */
            UpdateCurrentHitIndex();

            m_snapshot.valid = true;
        }

        debug.frameCount.fetch_add(1);
        debug.lastGameState.store(newSnap.gameState);
        debug.lastAudioTime.store(newSnap.audioTime);

        /* Poll rate: faster when playing (2ms), slower in menus (50ms) */
        Sleep(newSnap.isPlaying ? 2 : 50);
    }

    printf("[+] OsuReader: thread stopped\n");
}

/* ------------------------------------------------------------------ */
/*  Game State                                                         */
/* ------------------------------------------------------------------ */

bool OsuReader::ReadGameState()
{
    uint64_t statusPtr = m_sigs.GetStatusPtr();
    if (!statusPtr) return false;

    int32_t status = Read<int32_t>(statusPtr);

    /* Sanity check: status should be 0-15 */
    if (status < 0 || status > 15) return false;

    m_snapshot.gameState = status;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Audio Time                                                         */
/* ------------------------------------------------------------------ */

bool OsuReader::ReadAudioTime()
{
    uint64_t audioPtr = m_sigs.GetAudioTimePtr();
    if (!audioPtr) return false;

    int32_t time = Read<int32_t>(audioPtr);

    /* Sanity: audio time should be reasonable (-5000 to 600000 ms) */
    if (time < -5000 || time > 600000) return false;

    m_snapshot.audioTime = time;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Mods                                                               */
/* ------------------------------------------------------------------ */

bool OsuReader::ReadMods()
{
    uint64_t modsPtr = m_sigs.GetModsPtr();
    if (!modsPtr) return false;

    int32_t mods = Read<int32_t>(modsPtr);

    if (mods < 0 || mods > 0x1FFFF) return false;

    m_snapshot.mods = mods;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Beatmap Info                                                       */
/* ------------------------------------------------------------------ */

bool OsuReader::ReadBeatmapInfo()
{
    uint64_t basePtr = m_sigs.GetPlayingPtr();
    if (!basePtr) return false;

    uint32_t beatmapObj = Read<uint32_t>(basePtr);
    if (!beatmapObj || beatmapObj < 0x10000) return false;

    uint64_t bm = static_cast<uint64_t>(beatmapObj);
    m_snapshot.beatmap.AR = Read<float>(bm + 0x2C);
    m_snapshot.beatmap.CS = Read<float>(bm + 0x30);
    m_snapshot.beatmap.HP = Read<float>(bm + 0x34);
    m_snapshot.beatmap.OD = Read<float>(bm + 0x38);

    if (m_snapshot.beatmap.AR >= 0 && m_snapshot.beatmap.AR <= 13 &&
        m_snapshot.beatmap.CS >= 0 && m_snapshot.beatmap.CS <= 13 &&
        m_snapshot.beatmap.OD >= 0 && m_snapshot.beatmap.OD <= 13) {
        m_snapshot.beatmap.valid = true;
    }

    return m_snapshot.beatmap.valid;
}

/* ------------------------------------------------------------------ */
/*  Hit Objects (from .osu file)                                       */
/* ------------------------------------------------------------------ */

bool OsuReader::ReadHitObjects()
{
    if (m_hitObjectsCached)
        return true;

    /* Read beatmap folder + filename from the Beatmap object.
     * gosumemory offsets:
     *   Folder string = [[Beatmap] + 0x78]
     *   File   string = [[Beatmap] + 0x90]
     *
     * We also need the osu! Songs folder. We can try:
     *   1. Standard path: C:\Users\<user>\AppData\Local\osu!\Songs
     *   2. Read from settings (complex)
     *   3. Try common paths
     */
    uint64_t basePtr = m_sigs.GetPlayingPtr();
    if (!basePtr) return false;

    uint32_t beatmapObj = Read<uint32_t>(basePtr);
    if (!beatmapObj || beatmapObj < 0x10000) return false;

    uint64_t bm = static_cast<uint64_t>(beatmapObj);

    /* Read folder name and .osu filename */
    std::string folder = ReadDotNetString(bm + 0x78);
    std::string file   = ReadDotNetString(bm + 0x90);

    if (folder.empty() || file.empty()) {
        static bool once = false;
        if (!once) {
            printf("[!] ReadHitObjects: beatmap strings empty (folder='%s', file='%s')\n",
                   folder.c_str(), file.c_str());
            /* Try alternate offsets for the path */
            printf("[*] Probing beatmap strings:\n");
            for (uint32_t off = 0x60; off <= 0xC0; off += 0x4) {
                std::string s = ReadDotNetString(bm + off);
                if (!s.empty() && s.length() > 2) {
                    printf("    +0x%02X = \"%s\"\n", off, s.c_str());
                }
            }
            fflush(stdout);
            once = true;
        }
        return false;
    }

    /* Find the osu! Songs folder.
     * Try the standard location first, then try to find it
     * by walking up from the beatmap folder if needed. */
    std::string osuSongsPath;

    /* Try standard location */
    char appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableA("LOCALAPPDATA", appdata, MAX_PATH) > 0) {
        osuSongsPath = std::string(appdata) + "\\osu!\\Songs";
    }

    std::string fullPath = osuSongsPath + "\\" + folder + "\\" + file;

    /* Try opening the file */
    FILE* testFile = nullptr;
    fopen_s(&testFile, fullPath.c_str(), "r");
    if (!testFile) {
        /* Try alternate paths */
        const char* altPaths[] = {
            "C:\\osu!\\Songs",
            "D:\\osu!\\Songs",
            "E:\\osu!\\Songs",
            "C:\\Games\\osu!\\Songs",
            "D:\\Games\\osu!\\Songs",
        };
        for (const char* alt : altPaths) {
            std::string tryPath = std::string(alt) + "\\" + folder + "\\" + file;
            fopen_s(&testFile, tryPath.c_str(), "r");
            if (testFile) {
                fullPath = tryPath;
                break;
            }
        }
    }
    if (testFile) fclose(testFile);

    static bool loggedPath = false;
    if (!loggedPath) {
        printf("[*] ReadHitObjects: beatmap folder='%s' file='%s'\n",
               folder.c_str(), file.c_str());
        printf("[*] ReadHitObjects: trying path: %s\n", fullPath.c_str());
        fflush(stdout);
        loggedPath = true;
    }

    /* Parse the .osu file */
    if (!ParseOsuFile(fullPath)) {
        static bool once = false;
        if (!once) {
            printf("[!] ReadHitObjects: Failed to parse '%s'\n", fullPath.c_str());
            fflush(stdout);
            once = true;
        }
        return false;
    }

    debug.hitObjectsFound.store(m_snapshot.hitObjectCount);
    m_hitObjectsCached = true;

    printf("[+] OsuReader: %u hit objects loaded from file (first: t=%d, last: t=%d)\n",
           m_snapshot.hitObjectCount,
           m_snapshot.hitObjects[0].time,
           m_snapshot.hitObjects[m_snapshot.hitObjectCount - 1].time);
    fflush(stdout);

    return true;
}

/* ------------------------------------------------------------------ */
/*  Player State                                                       */
/* ------------------------------------------------------------------ */

bool OsuReader::ReadPlayerState()
{
    /* Corrected Ruleset chain: Read32(staticAddr + 0x4) */
    uint64_t rulesetBase = m_sigs.GetRulesetPtr();
    if (!rulesetBase) return false;

    uint32_t rulesetObj = Read<uint32_t>(rulesetBase + 0x4);
    if (!rulesetObj || rulesetObj < 0x10000) return false;

    uint32_t gameplayObj = Read<uint32_t>(static_cast<uint64_t>(rulesetObj) + 0x68);
    if (!gameplayObj || gameplayObj < 0x10000) return false;

    /* gosumemory: Score data at [[Ruleset + 0x68] + 0x38]
     * Combo at +0x94, Score at +0x78 */
    uint32_t scoreData = Read<uint32_t>(static_cast<uint64_t>(gameplayObj) + 0x38);
    if (scoreData && scoreData >= 0x10000) {
        uint64_t sd = static_cast<uint64_t>(scoreData);
        m_snapshot.combo = Read<int16_t>(sd + 0x94);
        m_snapshot.score = Read<int32_t>(sd + 0x78);
    }

    /* HP from [[Ruleset + 0x68] + 0x40] + 0x1C */
    uint32_t hpObj = Read<uint32_t>(static_cast<uint64_t>(gameplayObj) + 0x40);
    if (hpObj && hpObj >= 0x10000) {
        double hp = Read<double>(static_cast<uint64_t>(hpObj) + 0x1C);
        m_snapshot.playerHP = static_cast<float>(hp);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Derived Values                                                     */
/* ------------------------------------------------------------------ */

void OsuReader::ComputeHitWindows()
{
    float od = m_snapshot.beatmap.OD;

    if (m_snapshot.mods & OsuOffsets::MOD_HR) {
        od = (std::min)(od * 1.4f, 10.0f);
    }
    if (m_snapshot.mods & OsuOffsets::MOD_EZ) {
        od *= 0.5f;
    }

    m_snapshot.hitWindow300 = 80.0f - 6.0f * od;
    m_snapshot.hitWindow100 = 140.0f - 8.0f * od;
    m_snapshot.hitWindow50  = 200.0f - 10.0f * od;

    float cs = m_snapshot.beatmap.CS;
    if (m_snapshot.mods & OsuOffsets::MOD_HR)
        cs = (std::min)(cs * 1.3f, 10.0f);
    if (m_snapshot.mods & OsuOffsets::MOD_EZ)
        cs *= 0.5f;

    m_snapshot.circleRadius = 54.4f - 4.48f * cs;
}

void OsuReader::UpdateCurrentHitIndex()
{
    if (m_snapshot.hitObjectCount == 0) {
        m_snapshot.currentHitIndex = 0;
        return;
    }

    int32_t now = m_snapshot.audioTime;

    float passWindow = m_snapshot.hitWindow50 > 0 ? m_snapshot.hitWindow50 : 200.0f;

    for (uint32_t i = 0; i < m_snapshot.hitObjectCount; i++) {
        const HitObject& ho = m_snapshot.hitObjects[i];
        if (!ho.isValid) continue;

        int32_t effectiveEnd = ho.endTime > ho.time ? ho.endTime : ho.time;

        if (now < effectiveEnd + static_cast<int32_t>(passWindow)) {
            m_snapshot.currentHitIndex = i;
            return;
        }
    }

    /* All objects passed */
    m_snapshot.currentHitIndex = m_snapshot.hitObjectCount;
}
