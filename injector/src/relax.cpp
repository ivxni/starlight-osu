/*
 * starlight-osu :: injector/src/relax.cpp
 *
 * Humanized Relax assist.
 *
 * Key fixes over v1:
 *   - Index-based object tracking (no more re-processing or skipping)
 *   - Look-ahead: hold time is capped so close objects aren't missed
 *   - Missed objects are auto-skipped
 *   - Sliders properly held for their full duration
 *   - Gaussian hit timing, Log-Normal hold times, singletap/alternate
 */

#include "relax.h"
#include "kernel_input.h"
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <cctype>
#include <algorithm>

/* ------------------------------------------------------------------ */
/*  Construction / Reset                                               */
/* ------------------------------------------------------------------ */

Relax::Relax(KernelInput& input)
    : m_input(input)
    , m_rng(static_cast<unsigned>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count()))
{
}

Relax::~Relax()
{
    if (m_keyDown) ReleaseKey();
}

void Relax::Reset()
{
    if (m_keyDown) ReleaseKey();
    m_state          = ClickState::Idle;
    m_nextIdx        = 0;
    m_prevObjectTime = -1;
    m_pressTime      = 0;
    m_releaseTime    = 0;
    m_cooldownEnd    = 0;
    m_keyDown        = false;
    m_prevAudioTime  = -1;
    m_lastHitOffset  = 0.0f;
    m_acc.Reset();
    m_effectiveUR     = unstableRate;
    m_effectiveOffset = hitOffsetMean;
    m_loggedConfig    = false;
    m_loggedNoMore    = false;
    m_streamCatchUp   = 0.0f;
    m_lastWasStream   = false;
}

/* ------------------------------------------------------------------ */
/*  Statistical Samplers                                               */
/* ------------------------------------------------------------------ */

float Relax::SampleHitOffset()
{
    float stdDev = m_effectiveUR / 10.0f;
    if (stdDev < 1.0f) stdDev = 1.0f;
    std::normal_distribution<float> dist(m_effectiveOffset, stdDev);
    return dist(m_rng);
}

float Relax::SampleHoldTime(bool isStream)
{
    float mean = isStream ? holdMeanStream : holdMeanSingle;
    if (mean < 10.0f) mean = 10.0f;

    double vf = static_cast<double>(holdVariance);
    if (vf < 0.05) vf = 0.05;
    double sigmaSq = std::log(1.0 + vf * vf);
    double sigma   = std::sqrt(sigmaSq);
    double mu      = std::log(static_cast<double>(mean)) - 0.5 * sigmaSq;

    std::lognormal_distribution<double> dist(mu, sigma);
    float hold = static_cast<float>(dist(m_rng));

    hold = (std::max)(hold, 10.0f);
    hold = (std::min)(hold, 250.0f);
    return hold;
}

float Relax::SampleSliderRelease()
{
    float std = sliderReleaseStd;
    if (std < 1.0f) std = 1.0f;
    std::normal_distribution<float> dist(sliderReleaseMean, std);
    float v = dist(m_rng);
    v = (std::max)(v, -10.0f);
    v = (std::min)(v, 50.0f);
    return v;
}

/* ------------------------------------------------------------------ */
/*  Accuracy Controller                                                */
/* ------------------------------------------------------------------ */

void Relax::UpdateAccuracyController()
{
    if (targetAccuracy <= 0.0f) return;

    int total = m_acc.Total();
    if (total < 10 || (total % 10) != 0) return;

    float currentAcc = m_acc.Accuracy();
    float error = currentAcc - targetAccuracy;

    const float kP = 0.5f;

    if (error > 0.5f) {
        m_effectiveUR += kP * error;
        float drift = (m_effectiveOffset >= 0) ? 0.5f : -0.5f;
        m_effectiveOffset += drift;
    } else if (error < -0.5f) {
        m_effectiveUR += kP * error;
        m_effectiveOffset *= 0.95f;
    }

    m_effectiveUR     = (std::max)(m_effectiveUR, 50.0f);
    m_effectiveUR     = (std::min)(m_effectiveUR, 250.0f);
    m_effectiveOffset = (std::max)(m_effectiveOffset, -25.0f);
    m_effectiveOffset = (std::min)(m_effectiveOffset, 25.0f);

    if ((total % 100) == 0) {
        printf("[*] Relax/Acc: n=%d acc=%.2f%% tgt=%.1f%% UR=%.1f off=%.1f "
               "[300:%d 100:%d 50:%d x:%d]\n",
               total, currentAcc, targetAccuracy,
               m_effectiveUR, m_effectiveOffset,
               m_acc.count300, m_acc.count100, m_acc.count50, m_acc.countMiss);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/*  Key Helpers                                                        */
/* ------------------------------------------------------------------ */

uint16_t Relax::KeyToVK(const char* key)
{
    if (!key || !key[0]) return 'Z';
    char c = static_cast<char>(toupper(static_cast<unsigned char>(key[0])));
    return static_cast<uint16_t>(c);
}

void Relax::PressKey()
{
    if (m_keyDown) return;

    const char* k = m_useRightKey ? key2 : key1;
    uint16_t vk = KeyToVK(k);
    m_input.PressKey(vk);

    m_pressCount++;
    if (m_pressCount <= 10 || (m_pressCount % 100) == 0) {
        printf("[*] Relax: PressKey '%s' off=%.1fms #%d\n",
               k, m_lastHitOffset, m_pressCount);
        fflush(stdout);
    }
    m_keyDown = true;
}

void Relax::ReleaseKey()
{
    if (!m_keyDown) return;

    const char* k = m_useRightKey ? key2 : key1;
    uint16_t vk = KeyToVK(k);
    m_input.ReleaseKey(vk);
    m_keyDown = false;
}

/* ------------------------------------------------------------------ */
/*  Main Update                                                        */
/* ------------------------------------------------------------------ */

void Relax::Update(const OsuSnapshot& snap)
{
    if (!enabled || !snap.valid || !snap.isPlaying) {
        if (m_keyDown) ReleaseKey();
        if (snap.valid && !snap.isPlaying && m_state != ClickState::Idle)
            Reset();
        return;
    }

    int32_t now = snap.audioTime;

    /* Detect map restart (audio jumped backward >300 ms) */
    if (m_prevAudioTime >= 0 && now < m_prevAudioTime - 300) {
        if (m_keyDown) {
            ReleaseKey();
            uint16_t other = KeyToVK(m_useRightKey ? key1 : key2);
            m_input.ReleaseKey(other);
        }
        Reset();
        m_effectiveUR     = unstableRate;
        m_effectiveOffset = hitOffsetMean;
    }
    m_prevAudioTime = now;

    if (snap.hitObjectCount == 0) return;

    if (!m_loggedConfig) {
        m_effectiveUR     = unstableRate;
        m_effectiveOffset = hitOffsetMean;

        printf("[*] Relax: key1='%s' key2='%s' UR=%.0f offset=%.1f "
               "holdS=%.0f holdJ=%.0f var=%.2f "
               "slRel=%.0f+/-%.0f singleTap=%.0fms tgtAcc=%.1f%%\n",
               key1, key2, unstableRate, hitOffsetMean,
               holdMeanStream, holdMeanSingle, holdVariance,
               sliderReleaseMean, sliderReleaseStd,
               singletapThresholdMs, targetAccuracy);
        fflush(stdout);
        m_loggedConfig = true;
    }

    /* Keep our index in sync: if the game skipped ahead, catch up */
    if (snap.currentHitIndex > m_nextIdx)
        m_nextIdx = snap.currentHitIndex;

    /* Late-window: objects more than this many ms past their time are missed */
    float lateWindow = (snap.hitWindow50 > 0 ? snap.hitWindow50 : 200.0f) + 50.0f;

    switch (m_state) {

    /* ============================================================== */
    case ClickState::Idle: {

        /* Skip past objects that are already too late */
        while (m_nextIdx < snap.hitObjectCount) {
            const HitObject& ho = snap.hitObjects[m_nextIdx];
            if (!ho.isValid) { m_nextIdx++; continue; }

            int32_t effectiveEnd = (ho.endTime > ho.time) ? ho.endTime : ho.time;
            if (now > effectiveEnd + static_cast<int32_t>(lateWindow)) {
                m_nextIdx++;
                continue;
            }
            break;
        }

        if (m_nextIdx >= snap.hitObjectCount) {
            if (!m_loggedNoMore) {
                printf("[*] Relax: all %u objects processed (audio=%d)\n",
                       snap.hitObjectCount, now);
                fflush(stdout);
                m_loggedNoMore = true;
            }
            break;
        }

        const HitObject& next = snap.hitObjects[m_nextIdx];

        /* Gap-based stream detection */
        int32_t gap = (m_prevObjectTime >= 0)
                    ? (next.time - m_prevObjectTime) : 9999;
        bool isStream = (gap > 0 && gap < static_cast<int32_t>(singletapThresholdMs));

        /* Singletap vs Alternate: toggle key only in streams */
        if (isStream)
            m_useRightKey = !m_useRightKey;

        /* Sample hit offset (Gaussian) + stream catch-up when late */
        float hitOffset = SampleHitOffset() + globalOffsetMs;
        hitOffset += m_streamCatchUp;

        /* Decay catch-up: in streams decay slowly, else reset fast */
        m_streamCatchUp *= (isStream ? 0.75f : 0.3f);
        m_lastWasStream = isStream;
        m_lastHitOffset = hitOffset;
        m_pressTime = next.time + static_cast<int32_t>(hitOffset);

        /* Record predicted hit for accuracy tracking */
        float absOffset = std::abs(hitOffset);
        float w300 = snap.hitWindow300 > 0 ? snap.hitWindow300 : 40.0f;
        float w100 = snap.hitWindow100 > 0 ? snap.hitWindow100 : 100.0f;
        float w50  = snap.hitWindow50  > 0 ? snap.hitWindow50  : 150.0f;
        m_acc.Record(absOffset, w300, w100, w50);
        UpdateAccuracyController();

        /* Fatigue: slowly increase UR */
        m_effectiveUR += fatigueFactor / 100.0f;

        /* ---- Calculate release time ---- */
        if (next.isSlider && next.endTime > next.time) {
            float releaseOff = SampleSliderRelease();
            m_releaseTime = next.endTime - static_cast<int32_t>(releaseOff);
        } else if (next.isSpinner && next.endTime > next.time) {
            m_releaseTime = next.endTime;
        } else {
            float holdTime = SampleHoldTime(isStream);
            m_releaseTime = m_pressTime + static_cast<int32_t>(holdTime);
        }

        /* ---- Look-ahead: cap release so we don't block the next object ---- */
        if (!next.isSpinner) {
            for (uint32_t j = m_nextIdx + 1; j < snap.hitObjectCount; j++) {
                const HitObject& after = snap.hitObjects[j];
                if (!after.isValid) continue;

                int32_t cap = after.time - static_cast<int32_t>(minClickGapMs);
                if (m_releaseTime > cap) {
                    m_releaseTime = cap;
                }
                break;
            }
        }

        /* Absolute minimum hold (must be short enough for 300+ BPM) */
        if (m_releaseTime < m_pressTime + 10)
            m_releaseTime = m_pressTime + 10;

        m_prevObjectTime = next.time;
        m_nextIdx++;       /* advance past this object */
        m_state = ClickState::Pending;

        if (m_stateTransitions < 10) {
            printf("[*] Relax: schedule obj=%d press@%d rel@%d "
                   "(now=%d gap=%d %s %s)\n",
                   next.time, m_pressTime, m_releaseTime,
                   now, gap,
                   isStream ? "stream" : "single",
                   next.isSlider ? "SLIDER" : (next.isSpinner ? "SPIN" : "circle"));
            fflush(stdout);
        }
        break;
    }

    /* ============================================================== */
    case ClickState::Pending: {
        /* Check skip first (too late) - don't press, catch up next */
        if (now > m_pressTime + static_cast<int32_t>(lateWindow)) {
            if (m_lastWasStream) {
                float lateMs = static_cast<float>(now - m_pressTime);
                m_streamCatchUp = -(std::min)(lateMs * 0.4f, 20.0f);
            }
            m_state = ClickState::Idle;
            m_stateTransitions++;
        }
        else if (now >= m_pressTime) {
            /* Catch-up: if we pressed late in a stream, hit earlier next time */
            if (m_lastWasStream && now > m_pressTime) {
                float lateMs = static_cast<float>(now - m_pressTime);
                float correction = (std::min)(lateMs * 0.5f, 15.0f);
                m_streamCatchUp = -correction;
            }
            PressKey();
            m_state = ClickState::Holding;
            m_stateTransitions++;
        }
        break;
    }

    /* ============================================================== */
    case ClickState::Holding: {
        if (now >= m_releaseTime) {
            ReleaseKey();
            m_cooldownEnd = now + static_cast<int32_t>(minClickGapMs);
            m_state = ClickState::Cooldown;
            m_stateTransitions++;
        }
        break;
    }

    /* ============================================================== */
    case ClickState::Cooldown: {
        if (now >= m_cooldownEnd) {
            m_state = ClickState::Idle;
            m_stateTransitions++;
        }
        break;
    }

    } /* switch */
}
