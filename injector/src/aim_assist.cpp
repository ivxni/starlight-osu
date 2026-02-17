/*
 * starlight-osu :: injector/src/aim_assist.cpp
 *
 * Subtle aim correction using WindMouse humanizer.
 * Converts osu! playfield coords to screen space, computes deltas,
 * sends mouse movements via Windows SendInput.
 */

#include "aim_assist.h"
#include <Windows.h>
#include <cstdio>
#include <algorithm>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

AimAssist::AimAssist()
    : m_rng(static_cast<unsigned>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count()))
{
}

float AimAssist::RandFloat(float lo, float hi)
{
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(m_rng);
}

void AimAssist::Reset()
{
    m_windX = 0;
    m_windY = 0;
    m_velX = 0;
    m_velY = 0;
    m_lastTargetTime = -1;
    debugHasTarget = false;
}

/* ------------------------------------------------------------------ */
/*  Coordinate Conversion                                              */
/* ------------------------------------------------------------------ */

void AimAssist::OsuToScreen(float osuX, float osuY,
                             int screenW, int screenH,
                             float& screenX, float& screenY)
{
    float playH = static_cast<float>(screenH) * 0.8f;
    float playW = playH * (512.0f / 384.0f);

    if (playW > static_cast<float>(screenW) * 0.95f) {
        playW = static_cast<float>(screenW) * 0.8f;
        playH = playW * (384.0f / 512.0f);
    }

    float offX = (static_cast<float>(screenW) - playW) / 2.0f;
    float offY = (static_cast<float>(screenH) - playH) / 2.0f;

    screenX = offX + (osuX / 512.0f) * playW;
    screenY = offY + (osuY / 384.0f) * playH;
}

/* ------------------------------------------------------------------ */
/*  Per-Frame Update                                                   */
/* ------------------------------------------------------------------ */

void AimAssist::Update(const OsuSnapshot& snap, int screenW, int screenH)
{
    debugHasTarget = false;

    if (!enabled || !snap.valid || !snap.isPlaying) {
        if (!snap.isPlaying) Reset();
        return;
    }

    if (snap.hitObjectCount == 0) return;

    int32_t now = snap.audioTime;

    /* Find the next hit object to assist toward */
    const HitObject* target = nullptr;
    for (uint32_t i = snap.currentHitIndex; i < snap.hitObjectCount; i++) {
        const HitObject& ho = snap.hitObjects[i];
        if (!ho.isValid) continue;

        float timeDelta = static_cast<float>(ho.time - now);

        if (timeDelta < -lookBehindMs) continue;
        if (timeDelta > lookAheadMs) break;

        target = &ho;
        break;
    }

    if (!target) {
        m_windX *= 0.5f;
        m_windY *= 0.5f;
        return;
    }

    /* Convert target position to screen coordinates */
    float targetSX, targetSY;
    OsuToScreen(target->x, target->y, screenW, screenH, targetSX, targetSY);

    debugTargetX = targetSX;
    debugTargetY = targetSY;
    debugHasTarget = true;

    /* Get current cursor position */
    float cursorSX = static_cast<float>(screenW) / 2.0f;
    float cursorSY = static_cast<float>(screenH) / 2.0f;

    POINT cursorPos;
    if (GetCursorPos(&cursorPos)) {
        cursorSX = static_cast<float>(cursorPos.x);
        cursorSY = static_cast<float>(cursorPos.y);
    }

    /* Calculate delta in screen space */
    float dx = targetSX - cursorSX;
    float dy = targetSY - cursorSY;
    float dist = sqrtf(dx * dx + dy * dy);

    /* Convert screen distance back to approximate osu! pixel distance */
    float playH = static_cast<float>(screenH) * 0.8f;
    float scale = 384.0f / playH;
    float osuDist = dist * scale;

    /* Only assist if cursor is within assist radius (in osu! pixels) */
    if (osuDist > assistRadius) {
        m_windX *= 0.5f;
        m_windY *= 0.5f;
        return;
    }

    if (dist < 1.0f) return;

    /* Reset wind on target change */
    if (target->time != m_lastTargetTime) {
        m_windX = 0;
        m_windY = 0;
        m_velX = 0;
        m_velY = 0;
        m_lastTargetTime = target->time;
    }

    /* ---- Apply strength ---- */
    dx *= strength;
    dy *= strength;

    /* ---- Smoothing ---- */
    dx /= smoothing;
    dy /= smoothing;

    /* ---- WindMouse humanizer ---- */
    if (windMouseEnabled) {
        float gravity = RandFloat(gravityMin, gravityMax);
        float windStr = RandFloat(windMin, windMax);

        m_windX += RandFloat(-windStr, windStr);
        m_windY += RandFloat(-windStr, windStr);
        m_windX *= damping;
        m_windY *= damping;

        float gx = (dist > 1.0f) ? gravity * dx / dist : 0.0f;
        float gy = (dist > 1.0f) ? gravity * dy / dist : 0.0f;

        dx += (m_windX + gx) * 0.08f;
        dy += (m_windY + gy) * 0.08f;
    }

    /* ---- Easing (smoothstep when close to target) ---- */
    if (easingEnabled) {
        float normDist = (std::min)(osuDist / assistRadius, 1.0f);
        float ease = normDist * normDist * (3.0f - 2.0f * normDist);
        float factor = 0.2f + 0.8f * ease;
        dx *= factor;
        dy *= factor;
    }

    /* ---- Timing-based strength (stronger as hit time approaches) ---- */
    float timeDelta = static_cast<float>(target->time - now);
    if (timeDelta > 0 && lookAheadMs > 0) {
        float timeFactor = 1.0f - (timeDelta / lookAheadMs);
        timeFactor = (std::max)(0.0f, (std::min)(1.0f, timeFactor));
        timeFactor = timeFactor * timeFactor;
        dx *= timeFactor;
        dy *= timeFactor;
    }

    /* ---- Clamp ---- */
    dx = (std::max)(-50.0f, (std::min)(50.0f, dx));
    dy = (std::max)(-50.0f, (std::min)(50.0f, dy));

    /* ---- Send via Windows SendInput ---- */
    if (fabsf(dx) > 0.05f || fabsf(dy) > 0.05f) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>(dx);
        input.mi.dy = static_cast<LONG>(dy);
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(INPUT));
    }
}
