// STATUS: Active. Implements spec-findings-Actor-Radar-Layer.md. Interior
// cell walk + exterior grid walk both classify living actors by allegiance
// (Ally = IsPlayerTeammate; Animal = ActorTypeAnimal keyword and NOT
// IsInCombat; Hostile = IsHostileToActor(player); else Neutral) and hand back
// a flat by-value list. Animal is tested BEFORE Hostile as of 2026-08-26 --
// idle wildlife returns IsHostileToActor()==true (measured on a sandboxing
// Elk), so faction hostility alone painted nearly every critter red. No
// persistence, no cosave --
// see ActorMarkerSource.h for why this stays separate from EntityMarkerSource.
// Safety gate chain (Party Sheet precedent, findings section "Safety gates"):
// null / player-self / Is3DLoaded / !IsDead / !IsDisabled, in that exact
// order, BEFORE any classification virtual call -- cheap checks first so a
// freed/reused pointer short-circuits before the first vtable dispatch.
#include "ActorMarkerSource.h"
#include "Settings.h"

namespace {
    // User-tunable proximity radius ([NavigationOverlay] ActorProximityUnits),
    // falls back to the header default if Settings isn't up yet.
    float ProximityRadius() {
        auto* s = Settings::GetSingleton();
        return s ? s->navActorProximityUnits : ActorMarkerSource::kActorProximityRadius;
    }
}

namespace ActorMarkerSource {

namespace {
    // Per-hit gate + classify body, shared by both walk overloads (interior
    // ForEachReference callback and exterior ForEachReferenceInRange
    // callback), same shape as EntityMarkerSource::ClassifyAndAppend.
    constexpr std::size_t kMaxActors = 256;

    // "Is this actor actually swinging at the player right now."
    //
    // IsHostileToActor is faction/relationship state and stays FALSE for an NPC
    // who turns on the player without a faction change (bounty, brawl, Calm
    // expiring). IsInCombat() alone over-fires: it is true for a guard fighting
    // a spider across the room. The pair is what we want -- in combat AND the
    // combat target is the player.
    //
    // currentCombatTarget is a properly-typed ActorHandle in ACTOR_RUNTIME_DATA,
    // so it is SE/AE gated by the NG runtime-data macro. No hand offsets. Do NOT
    // swap this for CombatGroup::targets: CommonLib types that field as a bare
    // `int` (truncated, same trap as MapMenu+0x30470), so walking it means
    // hand-rolling a struct against unverified offsets for a dot color.
    bool IsFightingPlayer(RE::Actor* a_actor, RE::Actor* a_player) {
        if (!a_actor || !a_player) return false;
        if (!a_actor->IsInCombat()) return false;
        const RE::NiPointer<RE::Actor> target =
            a_actor->GetActorRuntimeData().currentCombatTarget.get();
        return target.get() == a_player;
    }

    RE::BSContainer::ForEachResult ClassifyAndAppend(RE::TESObjectREFR& ref,
                                                      RE::Actor* a_player,
                                                      const RE::NiPoint3& a_playerPos,
                                                      float a_radiusSq,
                                                      std::vector<ActorMarker>& out) {
        if (out.size() >= kMaxActors) return RE::BSContainer::ForEachResult::kStop;

        // Proximity gate: only actors near the player count. Cheapest check,
        // runs before the As<Actor> cast even. Radius^2 hoisted to the caller
        // so the Settings lookup isn't paid once per ref in the walk.
        const float dx = ref.GetPositionX() - a_playerPos.x;
        const float dy = ref.GetPositionY() - a_playerPos.y;
        if (dx * dx + dy * dy > a_radiusSq)
            return RE::BSContainer::ForEachResult::kContinue;

        auto* actor = ref.As<RE::Actor>();
        // Gate chain, in order (spec Safety gates #1-2): cheap null/identity
        // checks before ANY virtual call. A freed/reused pointer must
        // short-circuit here, or the crash lands inside IsDead()/Is3DLoaded().
        if (!actor) return RE::BSContainer::ForEachResult::kContinue;
        if (actor == a_player) return RE::BSContainer::ForEachResult::kContinue;
        if (!actor->Is3DLoaded()) return RE::BSContainer::ForEachResult::kContinue;
        if (actor->IsDead(true)) return RE::BSContainer::ForEachResult::kContinue;  // Corpse layer's job
        if (actor->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;  // garbage position

        // Classification. Animal is tested BEFORE Hostile, and IsInCombat is
        // what escalates a critter to red.
        //
        // The old order tested IsHostileToActor first and painted nearly all
        // wildlife red. That is not a data problem: measured live 2026-08-26,
        // an idle Prey-faction Elk (Spriggan Prey / Prey / Creature / Hunter
        // Prey, package DefaultCreatureSandbox) returns IsHostileToActor(player)
        // == true with IsInCombat() == false. IsHostileToActor is runtime
        // relationship state, not "wants to kill you", so it cannot carry the
        // red dot on its own.
        //
        // ActorTypeAnimal lives on the RACE, not the NPC record -- ElkRace and
        // WolfRace both carry it, EncFox's own keywords are only BestiaryElk-
        // style bestiary tags. Actor::HasKeywordString walks base then race
        // (TESNPC::HasApplicableKeywordString), so the race hop is already
        // covered; don't "fix" this by reading the race by hand.
        //
        // The mirror of that gap: IsHostileToActor is relationship state, so it
        // can miss an NPC who turns on the player without a faction change (a
        // brawl, Calm expiring, mod-driven combat). IsFightingPlayer covers that
        // by pairing IsInCombat with currentCombatTarget == player. IsInCombat
        // alone would over-fire -- a guard fighting a spider two rooms away is
        // in combat and is not the player's problem.
        //
        // Measured 2026-08-27 (Farmer + Whiterun Guard, direct assault): the two
        // signals flipped TOGETHER every time, 0 disagreements over a session.
        // Assaulting adds the "Player assaulted this NPC" faction, which makes
        // IsHostileToActor true on its own, so for direct assault the hostile
        // test alone was already sufficient. IsFightingPlayer is kept as belt-
        // and-braces for the no-assault-faction paths above, which are NOT yet
        // confirmed in a log. If it ever needs to justify its cost (one handle
        // read per actor per 2s scan), that is the evidence to go collect.
        //
        // Net: idle wolf/elk read as wildlife, a wolf that actually engages
        // reads red, a bandit reads red (no animal keyword, so it never enters
        // the Animal branch whatever its combat state), and a formerly-friendly
        // NPC flips to red the moment it targets the player.
        ActorMarker::Allegiance allegiance;
        const bool isAnimal = actor->HasKeywordString("ActorTypeAnimal");
        const bool fightingPlayer = IsFightingPlayer(actor, a_player);
        if (actor->IsPlayerTeammate()) {
            allegiance = ActorMarker::Allegiance::Ally;
        } else if (isAnimal && !actor->IsInCombat()) {
            allegiance = ActorMarker::Allegiance::Animal;
        } else if (fightingPlayer || (a_player && actor->IsHostileToActor(a_player))) {
            allegiance = ActorMarker::Allegiance::Hostile;
        } else {
            allegiance = ActorMarker::Allegiance::Neutral;
        }

        out.push_back({ actor->GetFormID(), ref.GetPosition(), allegiance });
        return RE::BSContainer::ForEachResult::kContinue;
    }
}

std::vector<ActorMarker> Collect(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_playerPos) {
    std::vector<ActorMarker> out;
    if (!a_cell) return out;
    out.reserve(32);

    auto* player = RE::PlayerCharacter::GetSingleton();
    const float r = ProximityRadius();
    const float r2 = r * r;
    a_cell->ForEachReference([&](RE::TESObjectREFR* ref) {
        if (!ref) return RE::BSContainer::ForEachResult::kContinue;
        return ClassifyAndAppend(*ref, player, a_playerPos, r2, out);
    });

    return out;
}

std::vector<ActorMarker> Collect(RE::TESObjectREFR* a_origin, float a_radius, const RE::NiPoint3& a_playerPos) {
    std::vector<ActorMarker> out;
    if (!a_origin) return out;
    out.reserve(32);

    auto* player = RE::PlayerCharacter::GetSingleton();
    const float r = ProximityRadius();
    const float r2 = r * r;

    // Hand-rolled grid walk, own copy of EntityMarkerSource.cpp:188-228 (spec
    // Open Question 1: duplicate, don't share -- extract only if a fix is
    // ever needed in both places). Do NOT use RE::TES::ForEachReferenceInRange:
    // its sky-cell tail dereferences a garbage worldSpace pointer and CTDs
    // (crash 2026-07-17, TESWorldSpace fn 20543). Cell-level
    // ForEachReferenceInRange is proven safe.
    auto* tes = RE::TES::GetSingleton();
    if (!tes) return out;
    const auto originPos = a_origin->GetPosition();
    const float xMin = originPos.x - a_radius, xMax = originPos.x + a_radius;
    const float yMin = originPos.y - a_radius, yMax = originPos.y + a_radius;
    if (auto* grid = tes->gridCells; grid && grid->length > 0) {
        const std::uint32_t len = grid->length;
        for (std::uint32_t x = 0; x < len; ++x) {
            for (std::uint32_t y = 0; y < len; ++y) {
                auto* cell = grid->GetCell(x, y);
                if (!cell || !cell->IsAttached()) continue;
                const auto* coords = cell->GetCoordinates();
                if (!coords) continue;
                // 4096 = exterior cell edge length in world units.
                if (coords->worldX >= xMax || coords->worldX + 4096.0f <= xMin ||
                    coords->worldY >= yMax || coords->worldY + 4096.0f <= yMin) continue;
                cell->ForEachReferenceInRange(originPos, a_radius,
                    [&](RE::TESObjectREFR* ref) {
                        if (!ref) return RE::BSContainer::ForEachResult::kContinue;
                        return ClassifyAndAppend(*ref, player, a_playerPos, r2, out);
                    });
            }
        }
    }

    return out;
}

} // namespace ActorMarkerSource
