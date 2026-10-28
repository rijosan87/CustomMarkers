#pragma once
#include <RE/Skyrim.h>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

// Inspect-overlay entity source: walks the player's current interior cell and
// classifies references (dead actors, loose loot, containers, doors,
// ingredients/plants) into a flat, copy-by-value list for the Navigation
// Overlay to draw. Standalone so
// the Glance radar / future surfaces can consume the same output later without
// touching the walker.
//
// Cadence is the CALLER's job: Collect runs inside RenderOneSnapshot only,
// never per frame (a per-frame cell walk cost ~30 fps, see LocalMapRenderer's
// beacon-cache note). Positions + FormIDs only leave the walk; a TESObjectREFR*
// is never held across frames (BeaconCache precedent).
//
// Proximity discovery (lore-friendly reveal): a glyph does not show until the
// player has once come within kEntityRevealRadius of the ref. After that it
// stays discovered for the session (per-entity fog of war). Discovery is folded
// into the same Collect walk, so there is no extra cadence or per-frame cost.
namespace EntityMarkerSource {
    enum class Category : std::uint8_t { Corpse, Door, Container, LooseLoot, Ingredient };

    // Icon-selection refinement within a Category. None means "no PNG match,
    // use the category fallback / vector glyph". Grouped by owning Category
    // for readability only; the enum itself is flat (draw side switches on
    // Category first, subcategory second).
    enum class SubCategory : std::uint8_t {
        None,
        // Container
        ContChest, ContBarrel, ContSack, ContUrn, ContCoffin, ContStrongbox,
        // Door
        DoorRegular, DoorLoad,
        // Corpse
        CorpseHumanoid, CorpseCreature,
        // Loose loot
        LootWeapon, LootArmor, LootGold, LootKey, LootPotion, LootFood, LootDrink, LootBook, LootSoulGem, LootScroll,
        // Ingredient
        IngrLoose, IngrPlant,
        // Appended at the END, not grouped with the other Loot* values: nothing
        // persists the numeric value (the per-subtype INI bools are keyed by
        // NAME in Settings.cpp), but appending keeps the diff to one line and
        // cannot disturb anything that walks the enum by index.
        LootAmmo,
        ContSatchel,
    };

    struct EntityMarker {
        Category     category;
        SubCategory  subCategory;
        RE::NiPoint3 position;
        RE::FormID   formID;      // runtime refr id, for future hover/dedup; not drawn
        bool         discovered;  // false = player has never been near it; draw skips it
    };

    // DEFAULT reveal radius in world units. ~one room: the player must physically
    // walk near a ref once before its glyph appears, so opening Inspect in a fresh
    // cell does not spoil the whole dungeon layout. Squared at the call site.
    // Glance and Inspect use this; loot beams pass their own (see below).
    constexpr float kEntityRevealRadius = 1000.0f;

    // Shortlist size. The walk keeps the NEAREST this many classified entities,
    // evicting the farthest once full, so it is a running best-N and not the
    // first N encountered. In the header (not file-local) so the perf log can
    // report whether a walk actually hit the cap.
    // MEASURED 2026-08-16 by temporarily dropping this to 20 so a ~20-classified
    // cell would overflow it: with eviction running every walk, the walk cost
    // stayed in its no-eviction band (42-52us vs 35-61us). The nearest-wins
    // max-scan is not worth replacing with a heap at any realistic density.
    constexpr std::size_t kMaxEntities = 256;

    // Walks a_cell's references and classifies them. a_playerPos drives the
    // proximity-discovery pass. Returns the NEAREST 256 classified entities,
    // each tagged with its current discovered state; the draw side gates on it.
    // The whole cell is always walked: the cap evicts the farthest rather than
    // stopping early, so a dense cell cannot starve a distance filter downstream.
    //
    // a_revealRadius overrides kEntityRevealRadius for THIS walk. Loot beams pass
    // lootBeamRevealUnits: sharing the 1000-unit constant silently capped their
    // range slider at ~14m, because an entity must pass reveal before range ever
    // sees it. The discovered SET is still shared and positive-only, so a ref
    // revealed by any consumer stays revealed for all of them.
    // a_requireLOS adds a line-of-sight test to the reveal gate: an entity in
    // range is only discovered if a Havok ray reaches it. Off by default so the
    // radar layers keep their pure distance fog. Loot beams pass it because a
    // beam popping in for a chest behind a wall reads as wrong -- the player has
    // not seen the thing yet. Cast only for in-range, not-yet-discovered
    // entities, so an explored room costs no rays at all.
    // a_rayBudget optionally queues that work and limits rays per Collect call;
    // without it, the original synchronous behavior is preserved.
    //
    // LOS-gated callers get their OWN discovered set. A single shared set cannot
    // hold two reveal policies: the un-gated radar walk would insert on distance
    // alone and hand the beam walk a reveal its own gate had just refused.
    //
    // a_filter allows early rejection at classification time before distance
    // calculations, line-of-sight raycasts, discovery hashing, or shortlist insertion.
    using FilterFn = bool (*)(Category, SubCategory, RE::TESObjectREFR&);

    struct PendingDiscoveryRay {
        RE::FormID  formID;
        Category    category;
        SubCategory subCategory;
    };

    // Optional first-reveal LOS scheduler owned by the caller. Pending FormIDs
    // survive collection ticks, and failed rays return to the back of the queue,
    // so a wall-blocked object cannot consume every tick and starve later loot.
    struct DiscoveryRayBudget {
        explicit DiscoveryRayBudget(std::size_t a_maxRaysPerCollect = 8) :
            maxRaysPerCollect(a_maxRaysPerCollect) {}

        std::size_t maxRaysPerCollect = 8;
        std::deque<PendingDiscoveryRay> pending;
        std::unordered_set<RE::FormID> queued;

        // Last-collect telemetry. The caller may log this behind its own gate.
        std::size_t filteredOut = 0;
        std::size_t candidates = 0;
        std::size_t raysCast   = 0;
        std::size_t discovered = 0;
        std::size_t targetSelfHits = 0;
        std::size_t deferred   = 0;
        double      rayUs      = 0.0;
        std::uint64_t generation = 0;
    };

    std::vector<EntityMarker> Collect(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_playerPos,
                                      float a_revealRadius = kEntityRevealRadius,
                                      bool a_requireLOS = false,
                                      FilterFn a_filter = nullptr,
                                      DiscoveryRayBudget* a_rayBudget = nullptr);

    // Exterior variant (spec-findings-Exterior-Navigation-Overlay.md decision 8):
    // outdoors there is no single cell to walk (the loaded area is a grid of
    // ~5x5 attached cells), so this radius-walks around a_origin instead via
    // RE::TES::ForEachReferenceInRange. Same classification body as the cell
    // walk (factored into a shared helper), same cap/discovery semantics.
    // a_origin is typically the player; a_radius is the WALK radius (how far out
    // to look for refs at all), a_revealRadius the discovery distance within it.
    std::vector<EntityMarker> Collect(RE::TESObjectREFR* a_origin, float a_radius, const RE::NiPoint3& a_playerPos,
                                      float a_revealRadius = kEntityRevealRadius,
                                      bool a_requireLOS = false,
                                      FilterFn a_filter = nullptr,
                                      DiscoveryRayBudget* a_rayBudget = nullptr);

    // Draw-side icon lookup, PNG basename (no extension/path). Tries the
    // subcategory first for a specific glyph (e.g. "barrel" vs generic
    // "container"); falls back to the category basename via CategoryIconName
    // if the subcategory has no dedicated art or is None. Returns nullptr
    // only when a_sub has no mapping (draw side then tries the category icon,
    // then the vector-glyph fallback).
    const char* IconNameFor(Category a_cat, SubCategory a_sub);

    // Category-level fallback PNG basename, used when IconNameFor(sub)
    // returns nullptr or the subcategory-specific PNG failed to load.
    const char* CategoryIconName(Category a_cat);

    // Wipe the session discovered-set. Called on kPostLoadGame / kNewGame so a
    // marker discovered in one playthrough does not carry false knowledge into
    // an older save. Cosave persistence is a planned later step; not built yet.
    void ClearDiscovered();
}
