#pragma once
/*
 * starlight-osu :: injector/src/config.h
 *
 * Config management: web backend (HTTP/JSON via localhost:8000/api/{uuid}/osu/config)
 * with INI file fallback. Same architecture as starlight-sys.
 */

#include "osu_reader.h"

class Relax;
class AimAssist;

/* Load config from local INI file (startup fallback).
 * Also loads [auth] uuid= for the web backend. */
void ConfigLoad(Relax* relax, AimAssist* aimAssist);

/* Save current config to local INI file (exit fallback). */
void ConfigSave(const Relax& relax, const AimAssist& aimAssist);

/* Start background thread that polls GET /api/{uuid}/osu/config every ~1s. */
void ConfigPollStart(Relax* relax, AimAssist* aimAssist);

/* Stop the polling thread. */
void ConfigPollStop();

/* Push live game state to the backend (POST /api/osu/gamestate).
 * Called from the render loop every frame; internally rate-limits to ~5 Hz. */
void GameStatePush(const OsuSnapshot& snap);

/* Get the loaded user UUID (empty string if not set). */
const char* ConfigGetUUID();
