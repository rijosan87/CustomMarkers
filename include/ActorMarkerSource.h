#pragma once

// Tracks nearby living actors (NPCs/creatures) for radar and inspect display.

#include <RE/Skyrim.h>
#include <cstdint>
#include <vector>

namespace ActorMarkerSource {
    struct ActorMarker {
        enum class Allegiance : std::uint8_t { Ally, Hostile, Neutral, Animal };
        RE::FormID   formID;
        RE::NiPoint3 position;
        Allegiance   allegiance;
    };

    constexpr float kActorProximityRadius = 1000.0f;

    std::vector<ActorMarker> Collect(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_playerPos);

    std::vector<ActorMarker> Collect(RE::TESObjectREFR* a_origin, float a_radius, const RE::NiPoint3& a_playerPos);
}
