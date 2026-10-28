#pragma once

// =====================================================================================
// STATUS — Local Map HUD Display Spike (live render via PreDisplay hook)
// -------------------------------------------------------------------------------------
// PURPOSE: Feasibility spike. Show the engine's kLOCAL_MAP render target as an ImGui
//          image on the gameplay HUD, and drive a LIVE refresh during gameplay.
//
// DISPLAY (Draw, Present hook): samples RENDER_TARGET::kLOCAL_MAP's SRV from
//          BSGraphics::Renderer and blits it into a small ImGui window. Display only.
//
// LIVE RENDER (HookedPreDisplay, HUDMenu::PreDisplay vtable idx 7): on the MAIN THREAD,
//          in the menu render phase, runs the LocalMapCullingProcess sequence (16333
//          construct once, camera setup faithfully replicating 52969's first half,
//          enable + TESCamera::Update tick, 16335 render) so kLOCAL_MAP refreshes during
//          gameplay. Driving 16335 from the Present hook (render thread) renders blank;
//          PreDisplay is the correct phase. See findings/findings-LocalMap-RenderPhase-
//          Entry.md and findings/findings-LocalMap-RenderDriver-Setup.md.
//          Trigger: press N (edge-triggered, one render per press). Gated so we don't cull
//          the scene every frame.
//
// AE-ONLY: HUDMenu IMenu PreDisplay = vtable idx 7 (RE::VTABLE_HUDMenu[0]). Render IDs:
//          16333 construct, 16335 render, 16327/16328/16330/16331 camera setup, cell
//          rotation via TESObjectCELL::GetNorthRotation (no Address Library anchor).
//
// ISOLATION: Standalone. (NOTE: Now parked. Rendering logic folded into MinimapRenderer per spec finding.)
// =====================================================================================

namespace LocalMapHudSpike {
    // Vtable-swaps HUDMenu::PreDisplay (IMenu idx 7) so the live render runs on the main
    // thread in the correct render phase. Call once at plugin load. See
    // findings/findings-LocalMap-RenderPhase-Entry.md.
    void Install();

    // Called every frame from the ImGui Present hook. Draws the kLOCAL_MAP target in a
    // small HUD window. No-op-safe if the target/SRV isn't available yet. Display only;
    // the render that fills kLOCAL_MAP is driven from the PreDisplay hook, not here.
    void Draw();
}
