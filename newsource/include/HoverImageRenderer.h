#pragma once

#include <RE/Skyrim.h>

namespace HoverImageRenderer {
    // Call once at plugin init (before ImGui starts). Parses
    // Data/SKSE/Plugins/CMM_MarkerImages.json into the in-memory FormID ->
    // image-stem map used by Draw(). Missing/malformed file is logged and
    // tolerated; the renderer falls back to no image.
    void Init();

    // Call once per MapMenu update frame, on the main thread, from inside
    // MarkerHoverHook::HookedUpdateHoveredMarker. Reads the active hover state
    // from Scaleform (selected idx + MarkerDescriptionHolder pos/scale/alpha
    // + viewport) and caches it for Draw() to consume on the render thread.
    void SampleCardState(RE::MapMenu* a_menu);

    // Call from ImGui::Renderer present hook (after MapMarkerRenderer::Draw).
    // Reads the cached state and draws the PNG image (looked up by FormID)
    // flush above the vanilla description card. If no image is mapped for
    // the FormID, draws nothing.
    void Draw();

    // Triggers the right-click context menu for the currently hovered marker
    void OpenContextMenu();

    // --- Local map (interior/city door icons) -------------------------------
    // The local map has its OWN marker array and its own hovered index, so the
    // world-map hover path above never sees a door. These mirror it for the
    // local map. See findings-LocalMap-Hover-Identity.md.

    // True while the local map is showing AND a vanilla marker (door) is under
    // the cursor. Drives the right-click routing in InputHook.
    bool IsLocalMarkerHovered();

    // Opens the local-map door context menu (Set/Remove Destination) for
    // whatever is hovered right now. Snapshots identity at call time.
    void OpenLocalContextMenu();

    // Returns the FormID of the currently hovered vanilla marker, or 0 if none.
    RE::FormID GetHoveredFormID();

    // Returns true if a vanilla marker is currently hovered AND the player has discovered it.
    bool IsHoveredDiscovered();

    // Returns true if a vanilla marker is currently hovered AND the player has NOT discovered it.
    bool IsHoveredUndiscovered();
}
