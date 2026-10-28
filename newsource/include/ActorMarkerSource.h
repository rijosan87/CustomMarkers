#pragma once
#include <RE/Skyrim.h>
#include <cstdint>
#include <vector>

// Inspect/Glance actor radar layer: walks the player's current interior cell
// (or an exterior radius grid) and classifies LIVING actors by allegiance for
// a transient, colored-dot HUD layer. Deliberately kept OUT of
// EntityMarkerSource (spec-findings-Actor-Radar-Layer.md decision 1):
// EntityMarkerSource's g_discovered is a session-persistent, insert-only set
// (fog of war for loot/doors/corpses), and actors here must forget entirely
// once they leave scan radius or die -- bolting a live/frozen/fading state
// onto that model would either make actors "discovered forever" or require a
// removal path the discovered-set was never designed for. So this module
// owns its own small walk (interior cell walk + exterior hand-rolled grid
// walk, mirroring EntityMarkerSource.cpp's two Collect overloads) and hands
// back a flat by-value list every ~2s snapshot cadence; the transient
// live/frozen/fade state itself lives on MinimapRenderer (_actorGhosts, see
// MinimapRenderer.h), not here -- Collect's job ends at "who's alive nearby
// and what's their allegiance right now".
//
// Cadence is the CALLER's job: Collect runs inside RenderOneSnapshot only,
// once per refresh (~2s), never per frame. No TESObjectREFR*/Actor* is held
// past the walk; only FormID/position/allegiance leave (same BeaconCache
// precedent EntityMarkerSource follows).
namespace ActorMarkerSource {
    struct ActorMarker {
        // Animal = non-hostile wildlife (fox/deer/rabbit etc.), its own color.
        // Hostile ALWAYS wins over Animal: a wolf aggro'd on the player reads
        // red, not the critter color (classify tests Hostile first).
        enum class Allegiance : std::uint8_t { Ally, Hostile, Neutral, Animal };
        RE::FormID   formID;
        RE::NiPoint3 position;
        Allegiance   allegiance;
    };

    // Default actor proximity radius in world units, ~one room. The LIVE value
    // is Settings::navActorProximityUnits ([NavigationOverlay]
    // ActorProximityUnits, slider "Detection Range"); this constant is only
    // the pre-Settings fallback and the documented default. An actor only
    // counts as "in proximity" (scanned live, dot moving) within the radius;
    // beyond it the ghost freezes and fades. Without this gate the interior
    // walk (whole cell) and exterior walk (Inspect range, 3000) both tracked
    // NPCs far into radar fog (2026-07-17 report).
    constexpr float kActorProximityRadius = 1000.0f;

    // Interior variant: walks a_cell's references, classifies only living,
    // loaded, non-player actors within kActorProximityRadius of a_playerPos.
    // Actors have no fog-of-war memory: transient, not session-persistent.
    std::vector<ActorMarker> Collect(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_playerPos);

    // Exterior variant: hand-rolled grid walk around a_origin (the player),
    // radius a_radius. Mirrors EntityMarkerSource.cpp:188-228's walk exactly
    // (own copy, not shared -- spec decision, Open Question 1: duplicate
    // rather than extract a shared helper until a fix is needed in both
    // places). Do NOT use RE::TES::ForEachReferenceInRange: its sky-cell tail
    // CTDs on AE 1.6.1170 (see EntityMarkerSource.cpp's comment + skse_patterns.md).
    std::vector<ActorMarker> Collect(RE::TESObjectREFR* a_origin, float a_radius, const RE::NiPoint3& a_playerPos);
}
