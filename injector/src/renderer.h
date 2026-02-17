#pragma once
/*
 * starlight-osu :: injector/src/renderer.h
 *
 * Debug overlay renderer for osu! assist.
 * Shows game state, timing info, hit object positions, and assist status.
 * Uses ImGui for drawing on the DX11 overlay window.
 */

#include "osu_reader.h"
#include "relax.h"
#include "aim_assist.h"
#include "imgui.h"

class OsuDebugRenderer {
public:
    OsuDebugRenderer() = default;

    void Render(const OsuSnapshot& snap, int screenW, int screenH,
                const Relax& relax, const AimAssist& aimAssist);

    /* Settings */
    bool showStatus    = false;   /* game state / audio time HUD (disabled) */
    bool showHitInfo   = true;    /* next hit object info */
    bool showPlayfield = false;   /* visualize hit objects on overlay */

private:
    /* Convert osu! playfield coords to screen coords */
    void OsuToScreen(float osuX, float osuY, int screenW, int screenH,
                     float& screenX, float& screenY);

    /* Render the status HUD panel */
    void RenderStatusHUD(const OsuSnapshot& snap, int screenW, int screenH,
                         const Relax& relax, const AimAssist& aimAssist);

    /* Render upcoming hit objects on overlay */
    void RenderHitObjects(const OsuSnapshot& snap, int screenW, int screenH);

    /* Render aim assist debug (target indicator) */
    void RenderAimDebug(const AimAssist& aimAssist, int screenW, int screenH);
};
