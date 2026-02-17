/*
 * starlight-osu :: injector/src/renderer.cpp
 *
 * Debug overlay renderer. Draws game state info and optional
 * hit object visualization using ImGui on the DX11 overlay.
 */

#include "renderer.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

/* ------------------------------------------------------------------ */
/*  Coordinate Conversion                                              */
/* ------------------------------------------------------------------ */

void OsuDebugRenderer::OsuToScreen(float osuX, float osuY,
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
/*  Main Render                                                        */
/* ------------------------------------------------------------------ */

void OsuDebugRenderer::Render(const OsuSnapshot& snap, int screenW, int screenH,
                               const Relax& relax, const AimAssist& aimAssist)
{
    if (!snap.valid) return;

    if (showStatus)
        RenderStatusHUD(snap, screenW, screenH, relax, aimAssist);

    if (showPlayfield && snap.isPlaying)
        RenderHitObjects(snap, screenW, screenH);

    if (aimAssist.showDebug && aimAssist.debugHasTarget)
        RenderAimDebug(aimAssist, screenW, screenH);
}

/* ------------------------------------------------------------------ */
/*  Status HUD                                                         */
/* ------------------------------------------------------------------ */

void OsuDebugRenderer::RenderStatusHUD(const OsuSnapshot& snap,
                                        int /*screenW*/, int /*screenH*/,
                                        const Relax& relax,
                                        const AimAssist& aimAssist)
{
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.6f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin("starlight-osu", nullptr, flags)) {
        /* Game state */
        const char* stateStr = "Unknown";
        ImU32 stateColor = IM_COL32(180, 180, 180, 255);
        switch (snap.gameState) {
            case 0: stateStr = "Menu"; break;
            case 1: stateStr = "Editing"; break;
            case 2: stateStr = "Playing";
                    stateColor = IM_COL32(100, 255, 100, 255); break;
            case 5: stateStr = "Song Select"; break;
            case 7: stateStr = "Ranking"; break;
            case 11: stateStr = "Multiplayer"; break;
        }

        ImGui::TextColored(ImVec4(
            static_cast<float>((stateColor >>  0) & 0xFF) / 255.0f,
            static_cast<float>((stateColor >>  8) & 0xFF) / 255.0f,
            static_cast<float>((stateColor >> 16) & 0xFF) / 255.0f,
            1.0f),
            "State: %s", stateStr);

        /* Audio time */
        int min = snap.audioTime / 60000;
        int sec = (snap.audioTime / 1000) % 60;
        int ms  = snap.audioTime % 1000;
        ImGui::Text("Audio: %d:%02d.%03d", min, sec, ms);

        /* Mods */
        if (snap.mods) {
            ImGui::Text("Mods: ");
            ImGui::SameLine();
            if (snap.mods & 8)   { ImGui::SameLine(); ImGui::Text("HD"); }
            if (snap.mods & 16)  { ImGui::SameLine(); ImGui::Text("HR"); }
            if (snap.mods & 64)  { ImGui::SameLine(); ImGui::Text("DT"); }
            if (snap.mods & 128) { ImGui::SameLine(); ImGui::Text("RX"); }
            if (snap.mods & 256) { ImGui::SameLine(); ImGui::Text("HT"); }
            if (snap.mods & 1)   { ImGui::SameLine(); ImGui::Text("NF"); }
            if (snap.mods & 2)   { ImGui::SameLine(); ImGui::Text("EZ"); }
        }

        /* Beatmap info */
        if (snap.beatmap.valid) {
            ImGui::Separator();
            ImGui::Text("AR:%.1f CS:%.1f OD:%.1f HP:%.1f",
                        snap.beatmap.AR, snap.beatmap.CS,
                        snap.beatmap.OD, snap.beatmap.HP);
            ImGui::Text("HitWindow: 300=%.0fms 100=%.0fms 50=%.0fms",
                        snap.hitWindow300, snap.hitWindow100, snap.hitWindow50);
        }

        /* Hit objects */
        if (snap.isPlaying && snap.hitObjectCount > 0) {
            ImGui::Separator();
            ImGui::Text("Objects: %u/%u",
                        snap.currentHitIndex, snap.hitObjectCount);

            /* Show next 3 hit objects */
            for (uint32_t i = snap.currentHitIndex;
                 i < snap.hitObjectCount && i < snap.currentHitIndex + 3; i++) {
                const HitObject& ho = snap.hitObjects[i];
                if (!ho.isValid) continue;

                int32_t delta = ho.time - snap.audioTime;
                const char* typeStr = ho.isCircle ? "O" :
                                      ho.isSlider ? "S" :
                                      ho.isSpinner ? "X" : "?";

                ImVec4 col(1.0f, 1.0f, 1.0f, 1.0f);
                if (delta < 0) col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                else if (delta < 100) col = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);

                ImGui::TextColored(col, "  %s (%.0f,%.0f) t=%+dms",
                                   typeStr, ho.x, ho.y, delta);
            }
        }

        /* Assist status */
        ImGui::Separator();
        ImGui::TextColored(relax.enabled
            ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
            : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Relax: %s", relax.enabled ? "ON" : "OFF");

        ImGui::SameLine();
        ImGui::TextColored(aimAssist.enabled
            ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
            : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            " Aim: %s", aimAssist.enabled ? "ON" : "OFF");

        /* Player state */
        if (snap.isPlaying) {
            ImGui::Text("HP: %.0f%% | Combo: %d | Score: %d",
                        snap.playerHP * 100.0f, snap.combo, snap.score);
        }
    }
    ImGui::End();
}

/* ------------------------------------------------------------------ */
/*  Hit Object Visualization                                           */
/* ------------------------------------------------------------------ */

void OsuDebugRenderer::RenderHitObjects(const OsuSnapshot& snap,
                                         int screenW, int screenH)
{
    if (snap.hitObjectCount == 0) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    /* Draw upcoming hit objects (next 10) */
    for (uint32_t i = snap.currentHitIndex;
         i < snap.hitObjectCount && i < snap.currentHitIndex + 10; i++) {
        const HitObject& ho = snap.hitObjects[i];
        if (!ho.isValid) continue;

        int32_t delta = ho.time - snap.audioTime;
        if (delta > 3000) break;

        float sx, sy;
        OsuToScreen(ho.x, ho.y, screenW, screenH, sx, sy);

        /* Scale radius based on CS */
        float radius = snap.circleRadius > 0 ? snap.circleRadius : 30.0f;
        float playH = static_cast<float>(screenH) * 0.8f;
        float screenRadius = (radius / 384.0f) * playH;

        /* Alpha based on approach timing */
        float alpha = 1.0f;
        if (delta > 0) {
            float ar_ms = 800.0f; /* approximate preempt time */
            alpha = 1.0f - (static_cast<float>(delta) / ar_ms);
            alpha = (std::max)(0.1f, (std::min)(1.0f, alpha));
        }

        uint8_t a = static_cast<uint8_t>(alpha * 200);

        if (ho.isCircle) {
            draw->AddCircle(ImVec2(sx, sy), screenRadius,
                            IM_COL32(255, 200, 100, a), 32, 2.0f);
        } else if (ho.isSlider) {
            draw->AddCircle(ImVec2(sx, sy), screenRadius,
                            IM_COL32(100, 200, 255, a), 32, 2.0f);
        } else if (ho.isSpinner) {
            draw->AddCircle(ImVec2(sx, sy), screenRadius * 2.0f,
                            IM_COL32(255, 100, 255, a), 32, 2.0f);
        }

        /* Time label */
        if (delta > -200) {
            char label[32];
            snprintf(label, sizeof(label), "%+dms", delta);
            draw->AddText(ImVec2(sx + screenRadius + 2, sy - 6),
                          IM_COL32(255, 255, 255, a), label);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Aim Assist Debug                                                   */
/* ------------------------------------------------------------------ */

void OsuDebugRenderer::RenderAimDebug(const AimAssist& aimAssist,
                                       int /*screenW*/, int /*screenH*/)
{
    if (!aimAssist.debugHasTarget) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    /* Draw crosshair on target */
    float tx = aimAssist.debugTargetX;
    float ty = aimAssist.debugTargetY;

    draw->AddCircle(ImVec2(tx, ty), 8.0f,
                    IM_COL32(255, 100, 100, 180), 16, 2.0f);
    draw->AddLine(ImVec2(tx - 12, ty), ImVec2(tx + 12, ty),
                  IM_COL32(255, 100, 100, 120), 1.0f);
    draw->AddLine(ImVec2(tx, ty - 12), ImVec2(tx, ty + 12),
                  IM_COL32(255, 100, 100, 120), 1.0f);
}
