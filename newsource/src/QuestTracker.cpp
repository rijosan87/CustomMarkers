#include "QuestTracker.h"
#include "HUDMarkerHook.h"
#include "Settings.h"

#include <set>
#include <tuple>

// ============================================================================
// QuestTracker — the journal-SELECTED quest set for the List panel.
// ----------------------------------------------------------------------------
// STATUS: per findings/spec-findings-Active-Quests-Section.md and
//   findings/spec-findings-Active-Miscellaneous-Objectives.md.
//   Regular quest tracked test = kActive && kDisplayedInHUD, both flags on
//   TESDataHandler::GetFormArray<TESQuest>(), excluding kMiscellaneous.
//   Confirmed in-game 2026-08-19 against the Journal Menu's own Flash
//   TitleList.entryList[].active: exactly the 4 journal-selected quests matched.
//   Neither flag alone works (kActive alone = ~15 incl. framework/bounty-template
//   quests; kDisplayedInHUD alone = all 20 running journal quests).
//
//   Miscellaneous objectives are tracked from PlayerCharacter::objectives
//   under the master miscellaneous category toggle (AE ID 406692, SE 1.5.97
//   raw RVA 0x1F4F090). Inclusion gates:
//     - ownerQuest->GetType() == kMiscellaneous
//     - entry.InstanceState == kDisplayed
//     - ownerQuest->data.flags.any(kActive)
//     - ownerQuest->currentInstanceID == entry.instanceID
//     - non-empty objective->displayText
//     - deduplicated by (questID, instanceID, objectiveIndex)
//   Objective displayText is expanded through the engine's quest-instance parser
//   (SE 23429 / AE 23897) so <Alias=...> tokens match the Journal Menu. Confirmed
//   in-game for miscellaneous objectives 2026-09-12; regular objectives use the
//   same parser with quest->currentInstanceID and await confirmation.
//
//   Positions resolve in FOUR passes, because HUDMarkerHook only sees markers
//   the engine can currently DRAW and an INTERIOR objective target never gets
//   one. The gate is interior, NOT distance: Wyrmstooth (target Athir,
//   ws=Tamriel) resolved while being FARTHER from the player than Bleak Falls
//   Barrow (target Farengar, inside Dragonsreach), which did not.
//     1. compass buffer, matched by quest FormID (regular only; disabled for
//        misc to prevent multi-objective quest ID collision).
//     2. unmatched rows fall back to the objective target's own position when
//        exterior, else the target LOCATION's worldLocMarker walking parentLoc.
//   Confirmed in-game 2026-08-19: Bleak Falls Barrow now resolves to
//   (21039,-7459,-3592), Whiterun's map icon, because Dragonsreach is an
//   interior with no world-map position of its own and parentLoc is Whiterun.
//   That is the intended answer for a map panel: the nearest place that exists
//   on the world map. Vanilla shows no Dragonsreach icon either.
//
//   Pass 4 covers what pass 3 cannot: a location chain with no MNAM anywhere.
//   Wyrmstooth is the case (WTBarrowLocation -> WTWyrmstoothLocation, a
//   LocTypeHold with no parent, neither carrying a marker), where pass 3 walks
//   off the end of the chain while the journal's own "Show on Map" still
//   resolves. TESQuestTarget carries the engine's load-door chain out of the
//   interior; walking it backwards and taking the first exterior door gives
//   the barrow entrance. See findings-Journal-ShowTargetOnMap.md. Offsets are
//   identical on SE 1.5.97 and AE 1.6.1170, so nothing here is version-gated.
//   Confirmed in-game 2026-08-26: "Barrow of the Wyrm" went from src=0 to
//   src=4, resolving door 08011916 in worldspace 08000D62, which matches the
//   open map so the row survives the worldspace filter and pans.
//
//   When pass 3 reaches only a LocTypeHold or LocTypeCity marker and the target
//   also has a valid exterior load door, the door is more precise and wins as
//   src=4. Without a valid door, the coarse marker remains the src=3 fallback.
//   Added 2026-09-16 for Dragonborn Gallery objectives resolving to Haafingar's
//   SolitudeEdgeMarker near Katla's Farm, then extended to city markers after
//   Bleak Falls Barrow resolved Farengar to CenterMarkerWhiterun instead of the
//   Dragonsreach entrance. Confirmed in-game 2026-09-16 on both quests.
//
//   A row can legitimately stay unclickable. "The Golden Claw" objective 35
//   ("Find the owner of the Golden Claw") is authored with target_count=0 in
//   Skyrim.esm, so there is nothing to point at and vanilla shows no marker.
//
//   NOT worldspace-scoped and NOT cache-gated: a journal edit produces no
//   worldspace signal, so a MapStats-style world guard would freeze the list.
//
//   Each resolved position DOES carry its worldSpaceID, and the List panel
//   drops rows that don't belong to the open map. Positions from different
//   worldspaces are in different coordinate spaces, so a Wyrmstooth quest
//   holding Tamriel coords used to pan to bare terrain on the Wyrmstooth map.
//   The compass-buffer path has no worldspace of its own: those markers only
//   exist for refs the engine can draw, i.e. the player's worldspace, so that
//   is what gets stamped.
// ============================================================================

// TESQuestTarget's teleport path, per findings-Journal-ShowTargetOnMap.md.
// CommonLib's TESQuestTarget is truncated (static_assert says 0x18) but the
// engine struct continues past that, so these are declared locally rather
// than borrowed from another class. Layout is 1:1 identical on SE 1.5.97 and
// AE 1.6.1170, so no REL::Relocate is needed on any field.
//
// Only populated for the objective the engine currently cares about: a
// COMPLETED objective's target reads all zeros from +0x10 to +0x60, which
// makes the whole layout look absent if that is the first one you inspect.
namespace {
    constexpr RE::FormID kLocTypeHoldLocalID = 0x00016771;
    constexpr RE::FormID kLocTypeCityLocalID = 0x00013168;

    struct TargetTeleportNode {
        bool               isExterior;    // +0x00
        char               pad01[7];      // +0x01
        RE::TESWorldSpace* worldspace;    // +0x08
        RE::TESObjectCELL* interiorCell;  // +0x10
    };
    static_assert(sizeof(TargetTeleportNode) == 0x18);

    struct TargetDoorNode {
        RE::TESObjectREFR* doorRef;  // +0x00
        std::uint64_t      unk08;    // +0x08
        std::uint64_t      unk10;    // +0x10
    };
    static_assert(sizeof(TargetDoorNode) == 0x18);

    struct TargetTeleportPath {
        RE::BSTArray<TargetTeleportNode> nodes;  // +0x00, i.e. target +0x18
        RE::BSTArray<TargetDoorNode>     doors;  // +0x18, i.e. target +0x30
    };

    [[nodiscard]] const bool* GetShowMiscObjectivesFlag()
    {
        if (REL::Module::IsAE()) {
            static REL::Relocation<const bool*> flag{ REL::ID(406692) };
            return flag.get();
        }

        // Skyrim SE 1.5.97 only. No Address Library row exists for this global.
        static REL::Relocation<const bool*> flag{ REL::Offset(0x1F4F090) };
        return flag.get();
    }

    [[nodiscard]] bool ShowMiscObjectives()
    {
        const auto* flag = GetShowMiscObjectivesFlag();
        return flag && *flag;
    }

    [[nodiscard]] bool IsCoarseMapLocation(RE::BGSLocation* a_location)
    {
        auto* holdKeyword = RE::TESForm::LookupByID<RE::BGSKeyword>(kLocTypeHoldLocalID);
        auto* cityKeyword = RE::TESForm::LookupByID<RE::BGSKeyword>(kLocTypeCityLocalID);
        return a_location &&
               ((holdKeyword && a_location->HasKeyword(holdKeyword)) ||
                (cityKeyword && a_location->HasKeyword(cityKeyword)));
    }

    [[nodiscard]] std::string FormatObjectiveText(
        RE::BGSQuestObjective* a_objective,
        const RE::TESQuest* a_quest,
        std::uint32_t a_instanceID)
    {
        if (!a_objective || !a_quest) {
            return {};
        }

        const char* raw = a_objective->displayText.c_str();
        if (!raw || !raw[0]) {
            return {};
        }

        // Skyrim's Journal Menu uses this parser for quest-instance substitutions
        // such as <Alias=Piglet> and <Alias.ShortName=Questgiver>. Do not replace
        // this pair with AE RVA 0x38AB30: its Address Library ID resolves to
        // BGSScene::WriteToSaveGame on SE.
        using ParseString_t = void(*)(RE::BSString*, const RE::TESQuest*, std::uint32_t);
        static REL::Relocation<ParseString_t> parseString{ REL::RelocationID(23429, 23897) };

        RE::BSString formatted{ a_objective->displayText };
        parseString(&formatted, a_quest, a_instanceID);

        const char* resolved = formatted.c_str();
        return resolved && resolved[0] ? resolved : raw;
    }

    void ResolveObjectivePosition(
        ActiveQuestRow& a_row,
        RE::TESQuest* a_quest,
        RE::BGSQuestObjective* a_objective,
        const std::vector<HUDMarkerHook::QuestMarker>& a_markers,
        RE::TESWorldSpace* a_playerWorld,
        bool a_allowQuestMarker)
    {
        if (!a_quest) {
            return;
        }

        if (a_allowQuestMarker) {
            for (const auto& m : a_markers) {
                if (m.questID != 0 && m.questID == a_row.questID) {
                    a_row.position     = m.position;
                    a_row.hasPosition  = true;
                    a_row.worldSpaceID = a_playerWorld ? a_playerWorld->GetFormID() : 0;
                    a_row.posSource    = 1;
                    break;
                }
            }
        }

        if (a_row.hasPosition || !a_objective) {
            return;
        }

        for (std::uint32_t t = 0; t < a_objective->numTargets; ++t) {
            auto* target = a_objective->targets ? a_objective->targets[t] : nullptr;
            if (!target) {
                continue;
            }

            auto refPtr = a_quest->GetAliasedRef(target->alias).get();
            if (!refPtr) {
                continue;
            }

            // Exterior target the compass simply had not drawn yet: its own
            // position is already world-valid, prefer it.
            auto* cell = refPtr->GetParentCell();
            if (cell && !cell->IsInteriorCell()) {
                auto* refWorld     = refPtr->GetWorldspace();
                a_row.position     = refPtr->GetPosition();
                a_row.hasPosition  = true;
                a_row.worldSpaceID = refWorld ? refWorld->GetFormID() : 0;
                a_row.posSource    = 2;
                break;
            }

            // Interior: collect the first valid location marker, but do not
            // commit it until the target's load-door path is also inspected.
            // City and hold markers are useful last resorts, but a
            // target-specific exterior door is more precise. Settlement and
            // dungeon markers retain the established src=3 behavior.
            RE::NiPoint3 locationPosition{};
            RE::FormID   locationWorldID      = 0;
            bool         hasLocationCandidate = false;
            bool         locationIsCoarse     = false;

            auto* loc   = refPtr->GetCurrentLocation();
            int   guard = 0;
            for (; loc && guard < 16; loc = loc->parentLoc, ++guard) {
                auto markerRef = loc->worldLocMarker.get();  // NiPointer, not raw
                if (!markerRef) {
                    continue;
                }

                auto* markerCell  = markerRef->GetParentCell();
                auto* markerWorld = markerRef->GetWorldspace();
                // A worldLocMarker can itself be interior. Using its cell-local
                // coordinates looks plausible after map-bound clamping, but pans
                // to the wrong location. Require an exterior cell and worldspace.
                if (markerCell && markerCell->IsInteriorCell()) {
                    continue;
                }
                if (!markerWorld) {
                    continue;
                }

                locationPosition     = markerRef->GetPosition();
                locationWorldID      = markerWorld->GetFormID();
                hasLocationCandidate = true;
                locationIsCoarse     = IsCoarseMapLocation(loc);
                break;
            }

            // Pass 4: collect the engine's target-specific exterior load door.
            // This remains the fallback when no location marker exists, and it
            // also supersedes a coarse city- or hold-level marker.
            RE::NiPoint3 doorPosition{};
            RE::FormID   doorWorldID      = 0;
            bool         hasDoorCandidate = false;

            if (!hasLocationCandidate || locationIsCoarse) {
                const auto* path = reinterpret_cast<const TargetTeleportPath*>(
                    reinterpret_cast<const std::uint8_t*>(target) + 0x18);

                for (std::int32_t i = static_cast<std::int32_t>(path->nodes.size()) - 1; i >= 0; --i) {
                    if (static_cast<std::size_t>(i) >= path->doors.size()) {
                        continue;
                    }

                    const auto& node    = path->nodes[static_cast<std::size_t>(i)];
                    auto*       doorRef = path->doors[static_cast<std::size_t>(i)].doorRef;
                    if (!node.isExterior || !doorRef) {
                        continue;
                    }

                    auto* doorWorld = doorRef->GetWorldspace();
                    if (!doorWorld) {
                        continue;
                    }

                    doorPosition     = doorRef->GetPosition();
                    doorWorldID      = doorWorld->GetFormID();
                    hasDoorCandidate = true;
                    break;
                }
            }

            const bool useDoor = hasDoorCandidate && (!hasLocationCandidate || locationIsCoarse);
            if (useDoor) {
                a_row.position     = doorPosition;
                a_row.hasPosition  = true;
                a_row.worldSpaceID = doorWorldID;
                a_row.posSource    = 4;
            } else if (hasLocationCandidate) {
                a_row.position     = locationPosition;
                a_row.hasPosition  = true;
                a_row.worldSpaceID = locationWorldID;
                a_row.posSource    = 3;
            }

            if (a_row.hasPosition) {
                break;
            }
        }
    }
}

QuestTracker* QuestTracker::GetSingleton() {
    static QuestTracker singleton;
    return &singleton;
}

void QuestTracker::Refresh() {
    std::vector<ActiveQuestRow> rows;

    const auto markers = HUDMarkerHook::GetQuestMarkers();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* playerWorld = player ? player->GetWorldspace() : nullptr;
    const bool qlog = Settings::GetSingleton()->questRowLog;

    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) {
        std::lock_guard lock(_mutex);
        _rows.clear();
        return;
    }

    for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
        if (!quest || !quest->IsRunning()) continue;

        // Skip miscellaneous quests - they are collected via the player objectives path.
        if (quest->GetType() == RE::QUEST_DATA::Type::kMiscellaneous) continue;

        // BOTH flags. See the STATUS block above for why neither alone works.
        if (!quest->data.flags.any(RE::QuestFlag::kActive)) continue;
        if (!quest->data.flags.any(RE::QuestFlag::kDisplayedInHUD)) continue;

        // No editor-ID fallback on purpose: it printed "DZ08_MelanaFollower"
        // during the probe and made a wrong-quest pick look like a working name
        // lookup. A nameless tracked quest (PetFramework_ParentQuest is the
        // observed case) is dropped rather than drawn as a blank row.
        const char* qname = quest->GetFullName();
        if (!qname || !qname[0]) continue;

        ActiveQuestRow row;
        row.questID   = quest->GetFormID();
        row.name      = qname;
        row.questType = static_cast<std::int32_t>(quest->GetType());

        // First DISPLAYED objective is the row's subtitle: the player wants the
        // current step, not the completed ones (kCompletedDisplayed).
        // objectives is a BSSimpleList, so range-for only, no size()/indexing.
        RE::BGSQuestObjective* firstObj = nullptr;
        for (auto* obj : quest->objectives) {
            if (!obj) continue;
            if (obj->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed) continue;
            firstObj = obj;
            row.objectiveText = FormatObjectiveText(obj, quest, quest->currentInstanceID);
            break;
        }

        ResolveObjectivePosition(row, quest, firstObj, markers, playerWorld, true);
        rows.push_back(std::move(row));
    }

    // Miscellaneous objectives collection path.
    // Ticked miscellaneous objectives are stored in PlayerCharacter::objectives.
    const bool masterOn = ShowMiscObjectives();
    if (player && masterOn) {
        using MiscKey = std::tuple<RE::FormID, std::uint32_t, std::uint16_t>;
        std::set<MiscKey> seen;

        for (const auto& entry : player->GetPlayerRuntimeData().objectives) {
            auto* objective = entry.Objective;
            auto* quest     = objective ? objective->ownerQuest : nullptr;

            if (!objective) {
                if (qlog) logger::info("[QuestMisc] quest=00000000 instance={} currentInstance=0 objective=0 state={} active=0 master=1 kept=0 reason=null-objective",
                                       entry.instanceID, static_cast<std::int32_t>(entry.InstanceState));
                continue;
            }
            if (!quest) {
                if (qlog) logger::info("[QuestMisc] quest=00000000 instance={} currentInstance=0 objective={} state={} active=0 master=1 kept=0 reason=null-quest",
                                       entry.instanceID, objective->index, static_cast<std::int32_t>(entry.InstanceState));
                continue;
            }
            if (quest->GetType() != RE::QUEST_DATA::Type::kMiscellaneous) {
                if (qlog) logger::info("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=non-misc",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }
            if (entry.InstanceState != RE::QUEST_OBJECTIVE_STATE::kDisplayed) {
                if (qlog) logger::info("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=not-displayed",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }
            if (!quest->data.flags.any(RE::QuestFlag::kActive)) {
                if (qlog) logger::info("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active=0 master=1 kept=0 reason=inactive",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState));
                continue;
            }
            if (quest->currentInstanceID != entry.instanceID) {
                if (qlog) logger::info("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=instance-mismatch",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }

            std::string text = FormatObjectiveText(objective, quest, entry.instanceID);
            if (text.empty()) {
                if (qlog) logger::info("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=empty-text",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }

            const MiscKey key{ quest->GetFormID(), entry.instanceID, objective->index };
            if (!seen.insert(key).second) {
                if (qlog) logger::info("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=duplicate",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }

            if (qlog) {
                logger::info("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active=1 master=1 kept=1 reason=accepted",
                             quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                             static_cast<std::int32_t>(entry.InstanceState));
            }

            ActiveQuestRow row;
            row.questID         = quest->GetFormID();
            row.objectiveText   = std::move(text);
            row.questType       = static_cast<std::int32_t>(quest->GetType());
            row.isMiscellaneous = true;
            row.instanceID      = entry.instanceID;
            row.objectiveIndex  = objective->index;

            ResolveObjectivePosition(row, quest, objective, markers, playerWorld, false);
            rows.push_back(std::move(row));
        }
    } else if (player && !masterOn && qlog) {
        logger::info("[QuestMisc] master=0 kept=0 reason=master-off");
    }

    if (qlog) {
        for (const auto& r : rows) {
            const char* kind = r.isMiscellaneous ? "misc" : "regular";
            const std::string& text = r.isMiscellaneous ? r.objectiveText : r.name;
            logger::info("[QuestRow] {:08X} \"{}\" kind={} instance={} objective={} hasPos={} world={:08X} src={}",
                         r.questID, text, kind, r.instanceID, r.objectiveIndex,
                         r.hasPosition, r.worldSpaceID, r.posSource);
        }
    }

    std::lock_guard lock(_mutex);
    _rows = std::move(rows);
}

std::vector<ActiveQuestRow> QuestTracker::Get() {
    std::lock_guard lock(_mutex);
    return _rows;
}
