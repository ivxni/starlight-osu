/*
 * starlight-osu :: injector/src/config.cpp
 *
 * Config persistence: web backend (HTTP/JSON) with INI file fallback.
 * Polls GET /api/{uuid}/osu/config from localhost:8000 every ~1s.
 * Pushes game state via POST /api/osu/gamestate at ~5 Hz.
 */

#include "config.h"
#include "relax.h"
#include "aim_assist.h"
#include <Windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#pragma comment(lib, "winhttp.lib")

/* ------------------------------------------------------------------ */
/*  Tiny JSON value extractor (no external library needed)             */
/* ------------------------------------------------------------------ */

static bool JsonBool(const char* json, const char* key, bool def)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == 't') return true;
    if (*p == 'f') return false;
    return def;
}

static int JsonInt(const char* json, const char* key, int def)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return atoi(p);
    return def;
}

static float JsonFloat(const char* json, const char* key, float def)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '-' || *p == '.' || (*p >= '0' && *p <= '9'))
        return static_cast<float>(atof(p));
    return def;
}

/* Extract sub-object by key */
static void ExtractSub(const char* json, const char* key, char* out, size_t outLen)
{
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '{') return;
    const char* start = p;
    int depth = 1;
    p++;
    while (*p && depth > 0) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    }
    size_t len = (size_t)(p - start);
    if (len >= outLen) len = outLen - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Apply JSON to relax + aim assist                                   */
/* ------------------------------------------------------------------ */

static void ApplyJson(const char* json, Relax* relax, AimAssist* aimAssist)
{
    char rxJson[2048], aimJson[2048];
    ExtractSub(json, "relax", rxJson, sizeof(rxJson));
    ExtractSub(json, "aim", aimJson, sizeof(aimJson));

    if (relax && rxJson[0]) {
        relax->enabled             = JsonBool(rxJson,  "enabled",             true);
        relax->targetAccuracy      = JsonFloat(rxJson, "targetAccuracy",      0.0f);
        relax->unstableRate        = JsonFloat(rxJson, "unstableRate",        80.0f);
        relax->hitOffsetMean       = JsonFloat(rxJson, "hitOffsetMean",       -2.0f);
        relax->holdMeanStream      = JsonFloat(rxJson, "holdMeanStream",      58.0f);
        relax->holdMeanSingle      = JsonFloat(rxJson, "holdMeanSingle",      72.0f);
        relax->holdMeanK2Factor    = JsonFloat(rxJson, "holdMeanK2Factor",    0.88f);
        relax->holdVariance        = JsonFloat(rxJson, "holdVariance",        0.28f);
        relax->sliderReleaseMean   = JsonFloat(rxJson, "sliderReleaseMean",   18.0f);
        relax->sliderReleaseStd    = JsonFloat(rxJson, "sliderReleaseStd",    8.0f);
        relax->singletapThresholdMs= JsonFloat(rxJson, "singletapThreshold",  125.0f);
        relax->minClickGapMs       = JsonFloat(rxJson, "minClickGap",         8.0f);
        relax->globalOffsetMs      = JsonFloat(rxJson, "globalOffset",        0.0f);
        relax->fatigueFactor       = JsonFloat(rxJson, "fatigueFactor",       1.5f);

        /* Legacy fields (still supported) */
        relax->jitterMinMs     = JsonFloat(rxJson, "jitterMin",     -8.0f);
        relax->jitterMaxMs     = JsonFloat(rxJson, "jitterMax",      12.0f);
        relax->holdMinMs       = JsonFloat(rxJson, "holdMin",       40.0f);
        relax->holdMaxMs       = JsonFloat(rxJson, "holdMax",       75.0f);
        relax->sliderReleaseMs = JsonFloat(rxJson, "sliderRelease", 15.0f);
        relax->alternateKeys   = JsonBool(rxJson,  "alternateKeys", true);

        /* Key strings */
        {
            char pattern[64];
            snprintf(pattern, sizeof(pattern), "\"key1\"");
            const char* p = strstr(rxJson, pattern);
            if (p) {
                p += strlen(pattern);
                while (*p == ' ' || *p == ':' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    const char* end = strchr(p, '"');
                    if (end && (end - p) < 8) {
                        memset(relax->key1, 0, sizeof(relax->key1));
                        memcpy(relax->key1, p, end - p);
                    }
                }
            }
            snprintf(pattern, sizeof(pattern), "\"key2\"");
            p = strstr(rxJson, pattern);
            if (p) {
                p += strlen(pattern);
                while (*p == ' ' || *p == ':' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    const char* end = strchr(p, '"');
                    if (end && (end - p) < 8) {
                        memset(relax->key2, 0, sizeof(relax->key2));
                        memcpy(relax->key2, p, end - p);
                    }
                }
            }
        }
    }

    if (aimAssist && aimJson[0]) {
        aimAssist->enabled          = JsonBool(aimJson,  "enabled",          false);
        aimAssist->strength         = JsonFloat(aimJson, "strength",         0.3f);
        aimAssist->assistRadius     = JsonFloat(aimJson, "assistRadius",     120.0f);
        aimAssist->lookAheadMs      = JsonFloat(aimJson, "lookAheadMs",      300.0f);
        aimAssist->lookBehindMs     = JsonFloat(aimJson, "lookBehindMs",     50.0f);
        aimAssist->smoothing        = JsonFloat(aimJson, "smoothing",        4.0f);
        aimAssist->windMouseEnabled = JsonBool(aimJson,  "windMouseEnabled", true);
        aimAssist->gravityMin       = JsonFloat(aimJson, "gravityMin",       3.0f);
        aimAssist->gravityMax       = JsonFloat(aimJson, "gravityMax",       7.0f);
        aimAssist->windMin          = JsonFloat(aimJson, "windMin",          0.5f);
        aimAssist->windMax          = JsonFloat(aimJson, "windMax",          3.0f);
        aimAssist->damping          = JsonFloat(aimJson, "damping",          0.8f);
        aimAssist->easingEnabled    = JsonBool(aimJson,  "easingEnabled",    true);
    }
}

/* ------------------------------------------------------------------ */
/*  HTTP GET via WinHTTP                                               */
/* ------------------------------------------------------------------ */

static std::string HttpGet(const wchar_t* host, INTERNET_PORT port,
                           const wchar_t* path)
{
    std::string result;

    HINTERNET session = WinHttpOpen(L"Starlight-osu/1.0",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;

    HINTERNET conn = WinHttpConnect(session, host, port, 0);
    if (!conn) { WinHttpCloseHandle(session); return result; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path,
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD timeout = 2000;
    WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);

    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr))
    {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            char buf[4096];
            DWORD bytesRead = 0;
            while (WinHttpReadData(req, buf, sizeof(buf) - 1, &bytesRead) &&
                   bytesRead > 0)
            {
                buf[bytesRead] = '\0';
                result += buf;
                bytesRead = 0;
            }
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

/* ------------------------------------------------------------------ */
/*  HTTP POST via WinHTTP                                              */
/* ------------------------------------------------------------------ */

static bool HttpPost(const wchar_t* host, INTERNET_PORT port,
                     const wchar_t* path, const std::string& body)
{
    HINTERNET session = WinHttpOpen(L"Starlight-osu/1.0",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    HINTERNET conn = WinHttpConnect(session, host, port, 0);
    if (!conn) { WinHttpCloseHandle(session); return false; }

    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path,
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD timeout = 1000;
    WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);

    LPCWSTR hdrs = L"Content-Type: application/json\r\n";
    bool ok = WinHttpSendRequest(req, hdrs, (DWORD)-1,
                                 (LPVOID)body.c_str(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0)
              && WinHttpReceiveResponse(req, nullptr);

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  User UUID                                                          */
/* ------------------------------------------------------------------ */

static char g_userUuid[128] = {};

const char* ConfigGetUUID() { return g_userUuid; }

/* ------------------------------------------------------------------ */
/*  Background Config Polling Thread                                   */
/* ------------------------------------------------------------------ */

static std::thread       g_pollThread;
static std::atomic<bool> g_pollRunning{false};
static Relax*            g_pollRelax    = nullptr;
static AimAssist*        g_pollAim      = nullptr;

static void PollThreadFunc()
{
    printf("[+] Config: HTTP poll thread started (localhost:8000/osu)\n");
    fflush(stdout);

    bool firstSuccess = false;

    while (g_pollRunning.load()) {
        wchar_t path[256];
        if (g_userUuid[0])
            _snwprintf_s(path, _countof(path), L"/api/%hs/osu/config", g_userUuid);
        else {
            /* No UUID - skip polling */
            Sleep(1000);
            continue;
        }

        std::string json = HttpGet(L"localhost", 8000, path);

        if (!json.empty()) {
            if (!firstSuccess) {
                printf("[+] Config: osu! web backend connected (uuid=%s)\n",
                       g_userUuid);
                fflush(stdout);
                firstSuccess = true;
            }

            ApplyJson(json.c_str(), g_pollRelax, g_pollAim);
        }

        for (int i = 0; i < 10 && g_pollRunning.load(); i++)
            Sleep(100);
    }

    printf("[+] Config: poll thread stopped\n");
    fflush(stdout);
}

void ConfigPollStart(Relax* relax, AimAssist* aimAssist)
{
    if (g_pollRunning.load()) return;

    g_pollRelax = relax;
    g_pollAim   = aimAssist;
    g_pollRunning.store(true);
    g_pollThread = std::thread(PollThreadFunc);
}

/* ------------------------------------------------------------------ */
/*  Game State Push (rate-limited POST to /api/osu/gamestate)          */
/* ------------------------------------------------------------------ */

static std::thread       g_pushThread;
static std::atomic<bool> g_pushRunning{false};
static OsuSnapshot       g_pushSnap;
static std::mutex        g_pushMutex;
static std::atomic<bool> g_pushReady{false};

static void PushThreadFunc()
{
    printf("[+] OsuGameState: push thread started\n");
    fflush(stdout);

    while (g_pushRunning.load()) {
        if (g_pushReady.load()) {
            OsuSnapshot snap;
            {
                std::lock_guard<std::mutex> lk(g_pushMutex);
                snap = g_pushSnap;
                g_pushReady.store(false);
            }

            char buf[512];
            snprintf(buf, sizeof(buf),
                "{\"gameState\":%d,\"audioTime\":%d,\"mods\":%d,"
                "\"combo\":%d,\"score\":%d,\"playerHP\":%.3f,"
                "\"beatmapAR\":%.1f,\"beatmapCS\":%.1f,"
                "\"beatmapOD\":%.1f,\"beatmapHP\":%.1f,"
                "\"hitObjectCount\":%u,\"currentHitIndex\":%u}",
                snap.gameState, snap.audioTime, snap.mods,
                snap.combo, snap.score, snap.playerHP,
                snap.beatmap.AR, snap.beatmap.CS,
                snap.beatmap.OD, snap.beatmap.HP,
                snap.hitObjectCount, snap.currentHitIndex);

            HttpPost(L"localhost", 8000, L"/api/osu/gamestate", buf);
        }

        Sleep(200);
    }

    printf("[+] OsuGameState: push thread stopped\n");
    fflush(stdout);
}

void GameStatePush(const OsuSnapshot& snap)
{
    if (!g_pushRunning.load()) {
        g_pushRunning.store(true);
        g_pushThread = std::thread(PushThreadFunc);
    }

    {
        std::lock_guard<std::mutex> lk(g_pushMutex);
        g_pushSnap = snap;
        g_pushReady.store(true);
    }
}

void ConfigPollStop()
{
    if (g_pollRunning.load()) {
        g_pollRunning.store(false);
        if (g_pollThread.joinable())
            g_pollThread.join();
    }
    if (g_pushRunning.load()) {
        g_pushRunning.store(false);
        if (g_pushThread.joinable())
            g_pushThread.join();
    }
}

/* ------------------------------------------------------------------ */
/*  INI File Fallback                                                  */
/* ------------------------------------------------------------------ */

static std::string GetConfigPath()
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
        return "slhost_osu_config.ini";

    std::string result(path);
    size_t last = result.rfind('\\');
    if (last == std::string::npos)
        last = result.rfind('/');
    if (last != std::string::npos)
        result.resize(last + 1);
    else
        result.clear();
    result += "slhost_osu_config.ini";
    return result;
}

static int ParseInt(const char* s, int def)
{
    if (!s || !*s) return def;
    return atoi(s);
}

static float ParseFloat(const char* s, float def)
{
    if (!s || !*s) return def;
    return static_cast<float>(atof(s));
}

static bool ParseBool(const char* s, bool def)
{
    if (!s || !*s) return def;
    if (*s == '1' || *s == 't' || *s == 'T') return true;
    if (*s == '0' || *s == 'f' || *s == 'F') return false;
    return def;
}

void ConfigLoad(Relax* relax, AimAssist* aimAssist)
{
    std::string path = GetConfigPath();
    FILE* f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, path.c_str(), "r");
#else
    f = fopen(path.c_str(), "r");
#endif
    if (!f) {
        printf("[*] Config: No INI file found (%s), using defaults\n", path.c_str());
        return;
    }

    char line[256];
    char section[64] = {};

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char* key = line;
            while (*key == ' ' || *key == '\t') key++;
            for (char* k = key; *k; k++)
                if (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') {
                    *k = '\0'; break;
                }
            char* val = eq + 1;
            while (*val == ' ' || *val == '\t') val++;
            for (char* v = val; *v; v++)
                if (*v == '\r' || *v == '\n') { *v = '\0'; break; }

            if (strcmp(section, "auth") == 0) {
                if (strcmp(key, "uuid") == 0)
                    strncpy(g_userUuid, val, sizeof(g_userUuid) - 1);
            } else if (strcmp(section, "relax") == 0 && relax) {
                if (strcmp(key, "enabled") == 0)            relax->enabled              = ParseBool(val, true);
                if (strcmp(key, "targetAccuracy") == 0)     relax->targetAccuracy       = ParseFloat(val, 0.0f);
                if (strcmp(key, "unstableRate") == 0)       relax->unstableRate         = ParseFloat(val, 100.0f);
                if (strcmp(key, "hitOffsetMean") == 0)      relax->hitOffsetMean        = ParseFloat(val, -3.0f);
                if (strcmp(key, "holdMeanStream") == 0)     relax->holdMeanStream       = ParseFloat(val, 58.0f);
                if (strcmp(key, "holdMeanSingle") == 0)     relax->holdMeanSingle       = ParseFloat(val, 72.0f);
                if (strcmp(key, "holdMeanK2Factor") == 0)   relax->holdMeanK2Factor     = ParseFloat(val, 0.88f);
                if (strcmp(key, "holdVariance") == 0)       relax->holdVariance         = ParseFloat(val, 0.28f);
                if (strcmp(key, "sliderReleaseMean") == 0)  relax->sliderReleaseMean    = ParseFloat(val, 20.0f);
                if (strcmp(key, "sliderReleaseStd") == 0)   relax->sliderReleaseStd     = ParseFloat(val, 10.0f);
                if (strcmp(key, "singletapThreshold") == 0) relax->singletapThresholdMs = ParseFloat(val, 180.0f);
                if (strcmp(key, "minClickGap") == 0)        relax->minClickGapMs        = ParseFloat(val, 15.0f);
                if (strcmp(key, "globalOffset") == 0)       relax->globalOffsetMs       = ParseFloat(val, 0.0f);
                if (strcmp(key, "fatigueFactor") == 0)      relax->fatigueFactor        = ParseFloat(val, 2.0f);
                if (strcmp(key, "key1") == 0)               strncpy(relax->key1, val, sizeof(relax->key1) - 1);
                if (strcmp(key, "key2") == 0)               strncpy(relax->key2, val, sizeof(relax->key2) - 1);
                /* Legacy fields */
                if (strcmp(key, "jitterMin") == 0)      relax->jitterMinMs     = ParseFloat(val, -8.0f);
                if (strcmp(key, "jitterMax") == 0)      relax->jitterMaxMs     = ParseFloat(val, 12.0f);
                if (strcmp(key, "holdMin") == 0)        relax->holdMinMs       = ParseFloat(val, 40.0f);
                if (strcmp(key, "holdMax") == 0)        relax->holdMaxMs       = ParseFloat(val, 75.0f);
                if (strcmp(key, "sliderRelease") == 0)  relax->sliderReleaseMs = ParseFloat(val, 15.0f);
                if (strcmp(key, "alternateKeys") == 0)  relax->alternateKeys   = ParseBool(val, true);
            } else if (strcmp(section, "aim") == 0 && aimAssist) {
                if (strcmp(key, "enabled") == 0)          aimAssist->enabled          = ParseBool(val, false);
                if (strcmp(key, "strength") == 0)         aimAssist->strength         = ParseFloat(val, 0.3f);
                if (strcmp(key, "assistRadius") == 0)     aimAssist->assistRadius     = ParseFloat(val, 120.0f);
                if (strcmp(key, "lookAheadMs") == 0)      aimAssist->lookAheadMs      = ParseFloat(val, 300.0f);
                if (strcmp(key, "lookBehindMs") == 0)     aimAssist->lookBehindMs     = ParseFloat(val, 50.0f);
                if (strcmp(key, "smoothing") == 0)        aimAssist->smoothing        = ParseFloat(val, 4.0f);
                if (strcmp(key, "windMouseEnabled") == 0) aimAssist->windMouseEnabled = ParseBool(val, true);
                if (strcmp(key, "gravityMin") == 0)       aimAssist->gravityMin       = ParseFloat(val, 3.0f);
                if (strcmp(key, "gravityMax") == 0)       aimAssist->gravityMax       = ParseFloat(val, 7.0f);
                if (strcmp(key, "windMin") == 0)          aimAssist->windMin          = ParseFloat(val, 0.5f);
                if (strcmp(key, "windMax") == 0)          aimAssist->windMax          = ParseFloat(val, 3.0f);
                if (strcmp(key, "damping") == 0)          aimAssist->damping          = ParseFloat(val, 0.8f);
                if (strcmp(key, "easingEnabled") == 0)    aimAssist->easingEnabled    = ParseBool(val, true);
            }
        }
    }

    fclose(f);
    printf("[+] Config loaded from %s (uuid=%s)\n", path.c_str(),
           g_userUuid[0] ? g_userUuid : "(none)");
}

void ConfigSave(const Relax& relax, const AimAssist& aimAssist)
{
    std::string path = GetConfigPath();
    FILE* f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, path.c_str(), "w");
#else
    f = fopen(path.c_str(), "w");
#endif
    if (!f) return;

    fprintf(f, "[auth]\n");
    fprintf(f, "uuid=%s\n\n", g_userUuid);

    fprintf(f, "[relax]\n");
    fprintf(f, "enabled=%d\n",            relax.enabled ? 1 : 0);
    fprintf(f, "key1=%s\n",               relax.key1);
    fprintf(f, "key2=%s\n",               relax.key2);
    fprintf(f, "targetAccuracy=%.1f\n",   relax.targetAccuracy);
    fprintf(f, "unstableRate=%.1f\n",     relax.unstableRate);
    fprintf(f, "hitOffsetMean=%.1f\n",    relax.hitOffsetMean);
    fprintf(f, "holdMeanStream=%.1f\n",   relax.holdMeanStream);
    fprintf(f, "holdMeanSingle=%.1f\n",   relax.holdMeanSingle);
    fprintf(f, "holdMeanK2Factor=%.2f\n", relax.holdMeanK2Factor);
    fprintf(f, "holdVariance=%.2f\n",     relax.holdVariance);
    fprintf(f, "sliderReleaseMean=%.1f\n",relax.sliderReleaseMean);
    fprintf(f, "sliderReleaseStd=%.1f\n", relax.sliderReleaseStd);
    fprintf(f, "singletapThreshold=%.1f\n",relax.singletapThresholdMs);
    fprintf(f, "minClickGap=%.1f\n",      relax.minClickGapMs);
    fprintf(f, "globalOffset=%.1f\n",     relax.globalOffsetMs);
    fprintf(f, "fatigueFactor=%.1f\n",    relax.fatigueFactor);

    fprintf(f, "\n[aim]\n");
    fprintf(f, "enabled=%d\n",          aimAssist.enabled ? 1 : 0);
    fprintf(f, "strength=%.2f\n",       aimAssist.strength);
    fprintf(f, "assistRadius=%.1f\n",   aimAssist.assistRadius);
    fprintf(f, "lookAheadMs=%.1f\n",    aimAssist.lookAheadMs);
    fprintf(f, "lookBehindMs=%.1f\n",   aimAssist.lookBehindMs);
    fprintf(f, "smoothing=%.1f\n",      aimAssist.smoothing);
    fprintf(f, "windMouseEnabled=%d\n", aimAssist.windMouseEnabled ? 1 : 0);
    fprintf(f, "gravityMin=%.2f\n",     aimAssist.gravityMin);
    fprintf(f, "gravityMax=%.2f\n",     aimAssist.gravityMax);
    fprintf(f, "windMin=%.2f\n",        aimAssist.windMin);
    fprintf(f, "windMax=%.2f\n",        aimAssist.windMax);
    fprintf(f, "damping=%.2f\n",        aimAssist.damping);
    fprintf(f, "easingEnabled=%d\n",    aimAssist.easingEnabled ? 1 : 0);

    fclose(f);
    printf("[+] Config saved to %s\n", path.c_str());
}
