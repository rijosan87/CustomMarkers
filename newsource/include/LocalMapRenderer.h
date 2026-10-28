#pragma once
#include <atomic>
#include <cstdint>
#include <unordered_map>

// Draws custom markers on the local map each ImGui frame.
//
// Architecture (see findings-localmap-click-re.md §4 + frame-timing note):
//
//   - Projection runs from a hook on LocalMapCamera::DefaultState::Update so it
//     fires AFTER the engine has propagated pan/zoom into the NiCamera's
//     worldToCam matrix this frame. Doing it from the ImGui Present hook race-
//     conditions against RefreshMarkers (which mutates the camera mid-frame).
//   - Projected screen positions are cached on this singleton; Draw() just
//     pulls from the cache and never touches the camera.
//
// Only markers whose cellID matches the player's current cell are projected.
class LocalMapRenderer {
public:
    static LocalMapRenderer* GetSingleton() {
        static LocalMapRenderer singleton;
        return &singleton;
    }

    void Install();
    void Draw();

    std::uint32_t GetRemoveButtonTarget() const { return _removeButtonTarget.load(); }
    std::uint32_t GetDestButtonTarget()   const { return _destButtonTarget.load(); }
    std::uint32_t GetEditButtonTarget()   const { return _editButtonTarget.load(); }

    // True when the cursor is hovering one of our custom pins on the local map.
    // Used by LocalMapClickHook to swallow clicks on existing pins so they don't
    // re-open the note dialog.
    bool IsCustomPinHovered() const { return _activeMarkerId != 0; }

    // id of the currently-hovered custom pin, or 0. InputHook's right-click
    // handler reads this to open MapMarkerRenderer::OpenCustomContextMenu for
    // the correct marker. _activeMarkerId is only written on the render
    // thread (Draw()), and InputHook runs on the game thread this frame's
    // value was already committed by, so a plain (non-atomic) read is fine
    // here — same assumption IsCustomPinHovered() above already makes.
    std::uint32_t GetHoveredMarkerId() const { return _activeMarkerId; }

    // True when the cursor is hovering the synthetic exit-door beacon (info
    // pin shown for an exterior destination while inside an interior). Used
    // by LocalMapClickHook to swallow the click so the note dialog doesn't
    // open at the beacon's spot.
    bool IsBeaconHovered() const { return _beaconHovered.load(); }

private:
    LocalMapRenderer() = default;
    ~LocalMapRenderer() = default;
    LocalMapRenderer(const LocalMapRenderer&) = delete;
    LocalMapRenderer& operator=(const LocalMapRenderer&) = delete;

    std::uint32_t              _activeMarkerId = 0;
    std::atomic<std::uint32_t> _removeButtonTarget{ 0 };
    std::atomic<std::uint32_t> _destButtonTarget{ 0 };
    std::atomic<std::uint32_t> _editButtonTarget{ 0 };
    std::atomic<bool>          _beaconHovered{ false };

    // Hover fade for custom pin tint: same easing scheme as MapMarkerRenderer,
    // kept per-renderer since the two are separate singletons with separate
    // pin sets.
    std::unordered_map<std::uint32_t, float> _hoverAnim;
};
