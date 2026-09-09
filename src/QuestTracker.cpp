#include "QuestTracker.h"
#include "HUDMarkerHook.h"
#include "Settings.h"

// STATUS: per findings/spec-findings-Active-Quests-Section.md.

namespace {
    struct TargetTeleportNode {
        bool               isExterior;
        char               pad01[7];
        RE::TESWorldSpace* worldspace;
        RE::TESObjectCELL* interiorCell;
    };
    static_assert(sizeof(TargetTeleportNode) == 0x18);

    struct TargetDoorNode {
        RE::TESObjectREFR* doorRef;
        std::uint64_t      unk08;
        std::uint64_t      unk10;
    };
    static_assert(sizeof(TargetDoorNode) == 0x18);

    struct TargetTeleportPath {
        RE::BSTArray<TargetTeleportNode> nodes;
        RE::BSTArray<TargetDoorNode>     doors;
    };
}

QuestTracker* QuestTracker::GetSingleton() {
    static QuestTracker singleton;
    return &singleton;
}

void QuestTracker::Refresh() {
    std::vector<ActiveQuestRow> rows;

    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) {
        std::lock_guard lock(_mutex);
        _rows.clear();
        return;
    }

    for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
        if (!quest || !quest->IsRunning()) continue;
        // BOTH flags. See the STATUS block above for why neither alone works.
        if (!quest->data.flags.any(RE::QuestFlag::kActive)) continue;
        if (!quest->data.flags.any(RE::QuestFlag::kDisplayedInHUD)) continue;
        const char* qname = quest->GetFullName();
        if (!qname || !qname[0]) continue;
        ActiveQuestRow row;
        row.questID   = quest->GetFormID();
        row.name      = qname;
        row.questType = static_cast<std::int32_t>(quest->GetType());
        for (auto* obj : quest->objectives) {
            if (!obj) continue;
            if (obj->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed) continue;
            if (const char* t = obj->displayText.c_str(); t && t[0]) {
                row.objectiveText = t;
            }
            break;
        }
        rows.push_back(std::move(row));
    }

    const auto markers = HUDMarkerHook::GetQuestMarkers();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* playerWorld = player ? player->GetWorldspace() : nullptr;
    for (auto& row : rows) {
        for (const auto& m : markers) {
            if (m.questID != 0 && m.questID == row.questID) {
                row.position     = m.position;
                row.hasPosition  = true;
                row.worldSpaceID = playerWorld ? playerWorld->GetFormID() : 0;
                row.posSource    = 1;
                break;
            }
        }
    }

    for (auto& row : rows) {
        if (row.hasPosition) continue;
        auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(row.questID);
        if (!quest) continue;
        for (auto* obj : quest->objectives) {
            if (!obj) continue;
            if (obj->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed) continue;
            for (std::uint32_t t = 0; t < obj->numTargets; ++t) {
                auto* target = obj->targets ? obj->targets[t] : nullptr;
                if (!target) continue;
                auto refPtr = quest->GetAliasedRef(target->alias).get();
                if (!refPtr) continue;
                auto* cell = refPtr->GetParentCell();
                if (cell && !cell->IsInteriorCell()) {
                    auto* refWorld  = refPtr->GetWorldspace();
                    row.position    = refPtr->GetPosition();
                    row.hasPosition = true;
                    row.worldSpaceID = refWorld ? refWorld->GetFormID() : 0;
                    row.posSource    = 2;
                    break;
                }
                auto* loc = refPtr->GetCurrentLocation();
                int   guard = 0;
                for (; loc && guard < 16; loc = loc->parentLoc, ++guard) {
                    auto markerRef = loc->worldLocMarker.get();
                    if (!markerRef) continue;
                    auto* markerCell = markerRef->GetParentCell();
                    auto* markerWorld = markerRef->GetWorldspace();
                    if (markerCell && markerCell->IsInteriorCell()) continue;
                    if (!markerWorld) continue;
                    row.position     = markerRef->GetPosition();
                    row.hasPosition  = true;
                    row.worldSpaceID = markerWorld->GetFormID();
                    row.posSource    = 3;
                    break;
                }
                if (!row.hasPosition) {
                    const auto* path = reinterpret_cast<const TargetTeleportPath*>(
                        reinterpret_cast<const std::uint8_t*>(target) + 0x18);
                    for (std::int32_t i = static_cast<std::int32_t>(path->nodes.size()) - 1; i >= 0; --i) {
                        if (static_cast<std::size_t>(i) >= path->doors.size()) continue;
                        const auto& node = path->nodes[static_cast<std::size_t>(i)];
                        auto* doorRef = path->doors[static_cast<std::size_t>(i)].doorRef;
                        if (!node.isExterior || !doorRef) continue;
                        auto* doorWorld = doorRef->GetWorldspace();
                        if (!doorWorld) continue;
                        row.position     = doorRef->GetPosition();
                        row.hasPosition  = true;
                        row.worldSpaceID = doorWorld->GetFormID();
                        row.posSource    = 4;
                        break;
                    }
                }
                if (row.hasPosition) break;
            }
            break;
        }
    }

    if (Settings::GetSingleton()->questRowLog) {
        for (const auto& r : rows) {
            logger::info("[QuestRow] {:08X} \"{}\" hasPos={} world={:08X} src={}",
                         r.questID, r.name, r.hasPosition, r.worldSpaceID, r.posSource);
        }
    }

    std::lock_guard lock(_mutex);
    _rows = std::move(rows);
}

std::vector<ActiveQuestRow> QuestTracker::Get() {
    std::lock_guard lock(_mutex);
    return _rows;
}
