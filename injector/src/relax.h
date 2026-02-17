#pragma once
/*
 * starlight-osu :: injector/src/relax.h
 *
 * Humanized Relax assist.
 *
 * Statistical models:
 *   - Hit timing:      Normal distribution (Gaussian), mean offset + UR.
 *   - Hold time:       Log-Normal distribution (realistic keystroke model),
 *                      different params for streams vs singles/jumps.
 *   - Slider release:  Normal distribution around the lenient window
 *                      (~36 ms before slider end), not frame-perfect.
 *   - Singletap/Alt:   Singletap for slow gaps, alternate for streams.
 *
 * Target Accuracy:
 *   Tracks internal 300/100/50/miss counts and dynamically adjusts UR
 *   and mean offset with a proportional controller to converge on the
 *   configured target accuracy by the end of the map.
 *
 * Fatigue Simulation:
 *   UR drifts upward slowly during the map (configurable rate).
 */

#include <cstdint>
#include <random>
#include "osu_reader.h"

class KernelInput;

class Relax {
public:
    explicit Relax(KernelInput& input);
    ~Relax();

    void Update(const OsuSnapshot& snap);
    void Reset();

    /* ---- Settings (configurable via config / web) ---- */
    bool  enabled              = true;

    char  key1[8]              = "Z";
    char  key2[8]              = "X";

    float targetAccuracy       = 0.0f;
    float unstableRate         = 80.0f;   // tight rhythm for speed player
    float hitOffsetMean        = -2.0f;

    float holdMeanStream       = 30.0f;   // fast enough for 300 BPM streams
    float holdMeanSingle       = 65.0f;
    float holdVariance         = 0.20f;

    float sliderReleaseMean    = 18.0f;
    float sliderReleaseStd     = 8.0f;

    float singletapThresholdMs = 125.0f;  // alternate earlier (speed player)
    float minClickGapMs        = 8.0f;    // tight gap for high BPM
    float globalOffsetMs       = 0.0f;
    float fatigueFactor        = 1.5f;    // lower fatigue (good stamina)

    /* Legacy fields kept for INI/web config compat */
    bool  alternateKeys        = true;
    float jitterMinMs          = -8.0f;
    float jitterMaxMs          = 12.0f;
    float holdMinMs            = 40.0f;
    float holdMaxMs            = 75.0f;
    float sliderReleaseMs      = 15.0f;

private:
    enum class ClickState { Idle, Pending, Holding, Cooldown };

    ClickState m_state = ClickState::Idle;

    bool m_useRightKey = false;

    /* Simple index-based object tracking.
     * m_nextIdx = index of the next object we haven't processed yet. */
    uint32_t m_nextIdx        = 0;
    int32_t  m_prevObjectTime = -1;

    int32_t  m_pressTime    = 0;
    int32_t  m_releaseTime  = 0;
    int32_t  m_cooldownEnd  = 0;
    bool     m_keyDown      = false;
    float    m_lastHitOffset = 0.0f;

    int32_t  m_prevAudioTime = -1;

    std::mt19937 m_rng;

    float SampleHitOffset();
    float SampleHoldTime(bool isStream);
    float SampleSliderRelease();

    void PressKey();
    void ReleaseKey();
    uint16_t KeyToVK(const char* key);

    KernelInput& m_input;

    /* ---- Accuracy Tracker ---- */
    struct AccuracyTracker {
        int count300 = 0, count100 = 0, count50 = 0, countMiss = 0;

        int   Total() const { return count300 + count100 + count50 + countMiss; }
        float Accuracy() const {
            int t = Total();
            if (t == 0) return 100.0f;
            return (300.0f * count300 + 100.0f * count100 + 50.0f * count50)
                   / (300.0f * t) * 100.0f;
        }

        void Record(float absOff, float w300, float w100, float w50) {
            if (absOff <= w300)      count300++;
            else if (absOff <= w100) count100++;
            else if (absOff <= w50)  count50++;
            else                     countMiss++;
        }

        void Reset() { count300 = count100 = count50 = countMiss = 0; }
    } m_acc;

    float m_effectiveUR     = 80.0f;
    float m_effectiveOffset = -2.0f;

    void UpdateAccuracyController();

    int  m_pressCount       = 0;
    int  m_stateTransitions = 0;
    bool m_loggedConfig     = false;
    bool m_loggedNoMore     = false;

    /* Stream catch-up: when late, hit earlier to get back in rhythm */
    float m_streamCatchUp   = 0.0f;
    bool  m_lastWasStream   = false;
};
