// STATUS: Active. Implements spec-findings-Actor-Radar-Layer.md. Interior

#include "ActorMarkerSource.h"
#include "Settings.h"

namespace {
    float ProximityRadius() {
        auto* s = Settings::GetSingleton();
        return s ? s->navActorProximityUnits : ActorMarkerSource::kActorProximityRadius;
    }
}

namespace ActorMarkerSource {
namespace {
    constexpr std::size_t kMaxActors = 256;
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
        const float dx = ref.GetPositionX() - a_playerPos.x;
        const float dy = ref.GetPositionY() - a_playerPos.y;
        if (dx * dx + dy * dy > a_radiusSq)
            return RE::BSContainer::ForEachResult::kContinue;
        auto* actor = ref.As<RE::Actor>();
        if (!actor) return RE::BSContainer::ForEachResult::kContinue;
        if (actor == a_player) return RE::BSContainer::ForEachResult::kContinue;
        if (!actor->Is3DLoaded()) return RE::BSContainer::ForEachResult::kContinue;
        if (actor->IsDead(true)) return RE::BSContainer::ForEachResult::kContinue;
        if (actor->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
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
}
