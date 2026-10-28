#pragma once

#include <vector>
#include "RE/Skyrim.h"

// Taps the engine's per-marker AddMarker call (RELOCATION_ID 50851/51728) to
// collect the world positions of quest/questDoor HUD markers each compass update.
// These are the engine's already-resolved, tracked-filtered, door-retargeted
// markers (NPC positions live). MinimapRenderer reads the buffer to draw active
// quest markers on Glance/Inspect. See findings-HUDMarker-Pipeline-QuestMarkers.md.
namespace HUDMarkerHook {
    void Install();

    // Publishes the accumulated quest markers as the completed pass. Call at the
    // end of each Compass::Update (from CompassHook's trampoline return), the real
    // per-frame boundary. Without this the pending buffer never becomes visible.
    void EndPass();

    // Snapshot of the last completed compass frame's quest-marker world positions.
    // Thread-safe: written on the game main thread inside AddMarker, read on the
    // Present thread by MinimapRenderer::BuildFrame.
    std::vector<RE::NiPoint3> GetQuestMarkerPositions();

    // Same markers, plus the owning quest where one could be resolved. The List
    // panel's Active Quests section uses this to pan to a tracked quest's live
    // marker; MinimapRenderer keeps using the positions-only call above.
    //
    // questID is 0 when the marker's ref belongs to no quest alias. That is
    // common and not an error: questDoor markers are engine-retargeted load
    // doors, which carry no alias at all.
    struct QuestMarker {
        RE::NiPoint3 position;
        RE::FormID   questID = 0;
    };
    std::vector<QuestMarker> GetQuestMarkers();
}
