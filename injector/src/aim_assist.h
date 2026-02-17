#pragma once
/*
 * starlight-osu :: injector/src/aim_assist.h
 *
 * Basic aim assist for osu!: applies subtle cursor correction toward
 * the next hit object. Uses WindMouse humanizer for natural movement.
 *
 * Works in osu! playfield coordinates (512x384), converts to screen
 * deltas, and sends mouse movement via Windows SendInput.
 */

#include <cstdint>
#include <cmath>
#include <random>
#include "osu_reader.h"

class AimAssist {
public:
    AimAssist();

    /*
     * Update - call every frame with latest game snapshot.
     * Reads cursor position and next hit object, applies correction.
     */
    void Update(const OsuSnapshot& snap, int screenW, int screenH);

    /* Reset state (on map change) */
    void Reset();

    /* ---- Settings ---- */
    bool  enabled          = false;   /* off by default, relax is primary */

    /* Correction strength (0.0 = off, 1.0 = full snap) */
    float strength         = 0.3f;

    /* Only assist when cursor is within this radius of target (osu! px) */
    float assistRadius     = 120.0f;

    /* Start assisting this many ms before hit time */
    float lookAheadMs      = 300.0f;

    /* Stop assisting after this many ms past hit time */
    float lookBehindMs     = 50.0f;

    /* WindMouse humanizer */
    bool  windMouseEnabled = true;
    float gravityMin = 3.0f, gravityMax = 7.0f;
    float windMin    = 0.5f, windMax    = 3.0f;
    float damping    = 0.8f;

    /* Smoothing (higher = slower cursor correction) */
    float smoothing        = 4.0f;

    /* Easing near target (smoothstep) */
    bool  easingEnabled    = true;

    /* Debug rendering */
    bool  showDebug        = false;
    float debugTargetX     = 0, debugTargetY = 0;
    bool  debugHasTarget   = false;

private:
    /* WindMouse state */
    float m_windX = 0, m_windY = 0;
    float m_velX = 0, m_velY = 0;

    /* Target tracking */
    int32_t m_lastTargetTime = -1;

    /* RNG */
    std::mt19937 m_rng;

    float RandFloat(float lo, float hi);

    void OsuToScreen(float osuX, float osuY, int screenW, int screenH,
                     float& screenX, float& screenY);
};
