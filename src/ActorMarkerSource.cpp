// STATUS: Active. Implements spec-findings-Actor-Radar-Layer.md. Interior

#include "ActorMarkerSource.h"
#include "Settings.h"

namespace {
    float ProximityRadius() {
        auto* s = Settings::GetSingleton();
        return s ? s->navActorProximityUnits : ActorMarkerSource::kActorProximityRadius;
    }

    float HostileRadius() {
        auto* s = Settings::GetSingleton();
        return s ? s->navHostileRangeUnits() : ActorMarkerSource::kActorProximityRadius;
    }
}

namespace ActorMarkerSource {
namespace {
    constexpr std::size_t kMaxActors = 256;
    bool IsTargetingPlayerSide(RE::Actor* a_actor, RE::Actor* a_player) {
        if (!a_actor || !a_player) return false;
        if (!a_actor->IsInCombat()) return false;
        const RE::NiPointer<RE::Actor> target =
            a_actor->GetActorRuntimeData().currentCombatTarget.get();
        return target && (target.get() == a_player || target->IsPlayerTeammate());
    }
    RE::BSContainer::ForEachResult ClassifyAndAppend(RE::TESObjectREFR& ref,
                                                      RE::Actor* a_player,
                                                      const RE::NiPoint3& a_playerPos,
                                                      float a_radiusSq,
                                                      float a_hostileRadiusSq,
                                                      std::vector<ActorMarker>& out) {
        if (out.size() >= kMaxActors) return RE::BSContainer::ForEachResult::kStop;
        const float dx = ref.GetPositionX() - a_playerPos.x;
        const float dy = ref.GetPositionY() - a_playerPos.y;
        const float distSq = dx * dx + dy * dy;
        if (distSq > std::max(a_radiusSq, a_hostileRadiusSq))
            return RE::BSContainer::ForEachResult::kContinue;
        auto* actor = ref.As<RE::Actor>();
        if (!actor) return RE::BSContainer::ForEachResult::kContinue;
        if (actor == a_player) return RE::BSContainer::ForEachResult::kContinue;
        if (!actor->Is3DLoaded()) return RE::BSContainer::ForEachResult::kContinue;
        if (actor->IsDead(true)) return RE::BSContainer::ForEachResult::kContinue;
        if (actor->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
        ActorMarker::Allegiance allegiance;
        const bool isAnimal = actor->HasKeywordString("ActorTypeAnimal");
        const bool engaged = IsTargetingPlayerSide(actor, a_player);
        if (actor->IsPlayerTeammate()) {
            allegiance = ActorMarker::Allegiance::Ally;
        } else if (isAnimal && !actor->IsInCombat()) {
            allegiance = ActorMarker::Allegiance::Animal;
        } else if (engaged || (a_player && actor->IsHostileToActor(a_player))) {
            allegiance = ActorMarker::Allegiance::Hostile;
        } else {
            allegiance = ActorMarker::Allegiance::Neutral;
        }

        if (distSq > (allegiance == ActorMarker::Allegiance::Hostile ? a_hostileRadiusSq : a_radiusSq))
            return RE::BSContainer::ForEachResult::kContinue;

        if (auto* settings = Settings::GetSingleton(); settings && settings->rimMarkerLog) {
            struct LoggedState {
                ActorMarker::Allegiance allegiance;
                bool engaged;
                bool searching;
                RE::FormID target;
                bool operator==(const LoggedState&) const = default;
            };
            static std::unordered_map<RE::FormID, LoggedState> lastLogged;
            if (lastLogged.size() > 512) lastLogged.clear();

            const auto& runtime = actor->GetActorRuntimeData();
            const RE::NiPointer<RE::Actor> target = runtime.currentCombatTarget.get();
            const LoggedState state{ allegiance, engaged,
                                     runtime.boolBits.any(RE::Actor::BOOL_BITS::kSearchingInCombat),
                                     target ? target->GetFormID() : 0 };
            const auto it = lastLogged.find(actor->GetFormID());
            if (it == lastLogged.end() || !(it->second == state)) {
                const auto stateName = [](const LoggedState& a_state) {
                    switch (a_state.allegiance) {
                    case ActorMarker::Allegiance::Ally:    return "Ally";
                    case ActorMarker::Allegiance::Animal:  return "Animal";
                    case ActorMarker::Allegiance::Neutral: return "Neutral";
                    default: return a_state.engaged ? "Red" : "Orange";
                    }
                };
                const char* name = actor->GetDisplayFullName();
                const char* targetName = target ? target->GetDisplayFullName() : nullptr;
                logger::info("[ActorState] {:08X} '{}' {} -> {} inCombat={} searching={} target={:08X} '{}'",
                             actor->GetFormID(), name ? name : "",
                             it == lastLogged.end() ? "new" : stateName(it->second), stateName(state),
                             actor->IsInCombat(), state.searching, state.target, targetName ? targetName : "");
                lastLogged[actor->GetFormID()] = state;
            }
        }

        ActorMarker marker{ actor->GetFormID(), ref.GetPosition(), allegiance };
        if (allegiance == ActorMarker::Allegiance::Hostile) {
            marker.engaged = engaged;
            const char* name = actor->GetDisplayFullName();
            marker.name = name ? name : "";
            marker.level = actor->GetLevel();
        }
        out.push_back(std::move(marker));
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
    const float hostileR = HostileRadius();
    const float hostileR2 = hostileR * hostileR;
    a_cell->ForEachReference([&](RE::TESObjectREFR* ref) {
        if (!ref) return RE::BSContainer::ForEachResult::kContinue;
        return ClassifyAndAppend(*ref, player, a_playerPos, r2, hostileR2, out);
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
    const float hostileR = HostileRadius();
    const float hostileR2 = hostileR * hostileR;
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
                        return ClassifyAndAppend(*ref, player, a_playerPos, r2, hostileR2, out);
                    });
            }
        }
    }
    return out;
}
}
