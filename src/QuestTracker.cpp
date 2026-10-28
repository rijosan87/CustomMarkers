#include "QuestTracker.h"
#include "HUDMarkerHook.h"
#include "Settings.h"

#include <SKSE/SKSE.h>
#include <Windows.h>
#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

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

    std::vector<std::string>* g_logBlock = nullptr;

    template <class... Args>
    void QLog(std::format_string<Args...> a_fmt, Args&&... a_args)
    {
        auto line = std::format(a_fmt, std::forward<Args>(a_args)...);
        if (g_logBlock) {
            g_logBlock->push_back(std::move(line));
        } else {
            logger::info("{}", line);
        }
    }

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

        using ParseString_t = void(*)(RE::BSString*, const RE::TESQuest*, std::uint32_t);
        static REL::Relocation<ParseString_t> parseString{ REL::RelocationID(23429, 23897) };

        RE::BSString formatted{ a_objective->displayText };
        parseString(&formatted, a_quest, a_instanceID);

        const char* resolved = formatted.c_str();
        return resolved && resolved[0] ? resolved : raw;
    }

    // Identity tokens for this game-task collection only. Never publish target
    // pointers to Present or hold the engine spinlock across reference lookup.
    std::vector<RE::TESQuestTarget*> CopyEligibleTargets(
        RE::PlayerCharacter* a_player, RE::TESQuest* a_quest, const char*& a_reason)
    {
        std::vector<RE::TESQuestTarget*> targets;
        if (!a_player || !a_quest) {
            a_reason = !a_player ? "missing-player" : "missing-quest";
            return targets;
        }

        auto& runtime = a_player->GetPlayerRuntimeData();
        {
            RE::BSSpinLockGuard lock(runtime.questTargetsLock);
            const auto it = runtime.questTargets.find(a_quest);
            if (it == runtime.questTargets.end()) {
                a_reason = "missing-entry";
            } else if (!it->second) {
                a_reason = "null-array";
            } else {
                a_reason = "empty-array";
                for (auto* target : *it->second) {
                    if (target) {
                        targets.push_back(target);
                    }
                }
                if (!targets.empty()) a_reason = "ready";
            }
        }
        return targets;
    }

    // Pass 5 result for one interior cell: the first exterior load door found
    // walking outward through linked load doors.
    struct InteriorExit {
        bool         found = false;
        RE::NiPoint3 position{};
        RE::FormID   worldID = 0;
        RE::FormID   doorID = 0;
        std::size_t  cellsWalked = 0;
    };

    const InteriorExit& FindInteriorExit(RE::TESObjectCELL* a_start)
    {
        static std::unordered_map<RE::FormID, InteriorExit> cache;
        if (const auto it = cache.find(a_start->GetFormID()); it != cache.end()) {
            return it->second;
        }

        InteriorExit result;
        std::vector<RE::TESObjectCELL*> queue{ a_start };
        std::unordered_set<RE::TESObjectCELL*> seen{ a_start };
        constexpr std::size_t kMaxCells = 16;
        for (std::size_t i = 0; i < queue.size() && i < kMaxCells && !result.found; ++i) {
            result.cellsWalked = i + 1;
            queue[i]->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                auto* teleport = a_ref ? a_ref->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
                RE::NiPointer<RE::TESObjectREFR> linked;
                if (teleport && teleport->teleportData) {
                    linked = teleport->teleportData->linkedDoor.get();
                }
                if (!linked) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (auto* world = linked->GetWorldspace()) {
                    result.found    = true;
                    result.position = linked->GetPosition();
                    result.worldID  = world->GetFormID();
                    result.doorID   = linked->GetFormID();
                    return RE::BSContainer::ForEachResult::kStop;
                }
                auto* linkedCell = linked->GetParentCell();
                if (linkedCell && linkedCell->IsInteriorCell() && seen.insert(linkedCell).second) {
                    queue.push_back(linkedCell);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        return cache.emplace(a_start->GetFormID(), result).first->second;
    }

    void ResolveObjectivePosition(
        ActiveQuestRow& a_row,
        RE::TESQuest* a_quest,
        RE::BGSQuestObjective* a_objective,
        const std::vector<HUDMarkerHook::QuestMarker>& a_markers,
        RE::TESWorldSpace* a_playerWorld,
        bool a_allowQuestMarker,
        const std::vector<RE::TESQuestTarget*>& a_eligibleTargets,
        const char* a_eligibilityReason)
    {
        if (!a_quest) {
            return;
        }

        const bool qlog = Settings::GetSingleton()->questRowLog;
        if (qlog && a_objective) {
            std::string authored;
            std::string eligible;
            for (std::uint32_t t = 0; t < a_objective->numTargets; ++t) {
                auto* target = a_objective->targets ? a_objective->targets[t] : nullptr;
                if (target) {
                    if (!authored.empty()) authored += ',';
                    authored += std::to_string(target->alias);
                }
            }
            for (auto* target : a_eligibleTargets) {
                if (!eligible.empty()) eligible += ',';
                eligible += std::to_string(target->alias);
            }
            QLog("[QuestTarget] quest={:08X} instance={} objective={} authored=[{}] eligible=[{}] reason={} thread={}",
                         a_row.questID, a_quest->currentInstanceID, a_objective->index,
                         authored, eligible, a_eligibilityReason, GetCurrentThreadId());
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

            const bool eligible = std::find(a_eligibleTargets.begin(), a_eligibleTargets.end(), target) !=
                                  a_eligibleTargets.end();
            if (qlog) {
                QLog("[QuestTarget] quest={:08X} instance={} objective={} alias={} eligible={} reason={}",
                             a_row.questID, a_quest->currentInstanceID, a_objective->index,
                             target->alias, eligible, eligible ? "accepted" : "ineligible");
            }
            // Target conditions can select a different entrance without changing
            // objective order. Alias/ref identity alone cannot express that choice.
            if (!eligible) {
                continue;
            }

            auto refPtr = a_quest->GetAliasedRef(target->alias).get();
            if (qlog) {
                QLog("[QuestTarget] quest={:08X} instance={} objective={} alias={} ref={:08X} reason={}",
                             a_row.questID, a_quest->currentInstanceID, a_objective->index,
                             target->alias, refPtr ? refPtr->GetFormID() : 0,
                             refPtr ? "alias-resolved" : "null-alias-ref");
            }
            if (!refPtr) {
                continue;
            }

            // GetWorldspace() falls back to GetSaveParentCell(), so distant
            // exterior targets still resolve when parentCell is not loaded.
            if (auto* refWorld = refPtr->GetWorldspace()) {
                a_row.position     = refPtr->GetPosition();
                a_row.hasPosition  = true;
                a_row.worldSpaceID = refWorld->GetFormID();
                a_row.posSource    = 2;
                break;
            }

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

            RE::NiPoint3 doorPosition{};
            RE::FormID   doorWorldID      = 0;
            bool         hasDoorCandidate = false;

            if (!hasLocationCandidate || locationIsCoarse) {
                const auto* path = reinterpret_cast<const TargetTeleportPath*>(
                    reinterpret_cast<const std::uint8_t*>(target) + 0x18);

                const bool pathEndsInterior =
                    !path->nodes.empty() && !path->nodes.back().isExterior;

                if (pathEndsInterior && !path->doors.empty()) {
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
            }

            if (qlog) {
                const auto* path = reinterpret_cast<const TargetTeleportPath*>(
                    reinterpret_cast<const std::uint8_t*>(target) + 0x18);
                auto* refCell = refPtr->GetParentCell();
                auto* refLoc  = refPtr->GetCurrentLocation();
                std::string nodes;
                for (const auto& n : path->nodes) {
                    if (!nodes.empty()) nodes += ',';
                    nodes += n.isExterior ? 'E' : 'I';
                }
                QLog("[QuestPass] quest={:08X} objective={} alias={} ref={:08X} cell={:08X} interior={} loc={:08X} locCandidate={} coarse={} nodes={}[{}] doors={} doorCandidate={} doorWorld={:08X}",
                             a_row.questID, a_objective->index, target->alias, refPtr->GetFormID(),
                             refCell ? refCell->GetFormID() : 0,
                             refCell ? refCell->IsInteriorCell() : false,
                             refLoc ? refLoc->GetFormID() : 0,
                             hasLocationCandidate, locationIsCoarse,
                             path->nodes.size(), nodes, path->doors.size(),
                             hasDoorCandidate, doorWorldID);
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
            } else if (auto* targetCell = refPtr->GetParentCell(); targetCell && targetCell->IsInteriorCell()) {
                const auto& exit = FindInteriorExit(targetCell);
                if (qlog) {
                    QLog("[QuestPass] quest={:08X} objective={} pass5 cell={:08X} found={} door={:08X} world={:08X} cellsWalked={}",
                                 a_row.questID, a_objective->index, targetCell->GetFormID(),
                                 exit.found, exit.doorID, exit.worldID, exit.cellsWalked);
                }
                if (exit.found) {
                    a_row.position     = exit.position;
                    a_row.hasPosition  = true;
                    a_row.worldSpaceID = exit.worldID;
                    a_row.posSource    = 5;
                }
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
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) {
        return;
    }

    std::uint64_t generation;
    {
        std::lock_guard lock(_mutex);
        _sessionActive = true;
        if (_refreshPending) {
            return;
        }
        _refreshPending = true;
        generation = _generation;
    }
    // Debug level: fires every frame while the map is open.
    if (Settings::GetSingleton()->questRowLog) {
        logger::debug("[QuestRefresh] generation={} phase=request thread={}", generation, GetCurrentThreadId());
    }
    tasks->AddTask([this, generation]() {
        RefreshOnGameThread(generation);
    });
}

void QuestTracker::Reset() {
    std::uint64_t generation;
    bool pending;
    {
        std::lock_guard lock(_mutex);
        if (!_sessionActive) {
            return;
        }
        _sessionActive = false;
        generation = ++_generation;
        _rows.clear();
        // Keep the slot claimed until the obsolete task retires. Close/reopen
        // must not build a queue of tasks while the paused map isn't servicing it.
        pending = _refreshPending;
    }
    if (Settings::GetSingleton()->questRowLog) {
        logger::info("[QuestRefresh] generation={} phase=reset pending={} thread={}",
                     generation, pending, GetCurrentThreadId());
    }
}

void QuestTracker::RefreshOnGameThread(std::uint64_t a_generation) {
    const bool qlog = Settings::GetSingleton()->questRowLog;
    const auto complete = [&](std::vector<ActiveQuestRow> a_rows, bool a_publish, const char* a_reason) {
        if (a_publish) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
                Reset();
                a_publish = false;
                a_reason = "map-closed";
            }
        }
        const auto count = a_rows.size();
        {
            std::lock_guard lock(_mutex);
            if (a_generation != _generation || !_sessionActive) {
                a_publish = false;
                a_reason = "stale-generation";
            }
            if (a_publish) {
                _rows = std::move(a_rows);
            }
            _refreshPending = false;
        }
        if (qlog) {
            // A routine published refresh repeats every frame: debug only.
            // Anything else (stale, map closed, missing player) stays at info.
            if (a_publish && std::string_view(a_reason) == "collected") {
                logger::debug("[QuestRefresh] generation={} phase=complete published={} rows={} reason={} thread={}",
                              a_generation, a_publish, count, a_reason, GetCurrentThreadId());
            } else {
                logger::info("[QuestRefresh] generation={} phase=complete published={} rows={} reason={} thread={}",
                             a_generation, a_publish, count, a_reason, GetCurrentThreadId());
            }
        }
    };

    if (qlog) {
        logger::debug("[QuestRefresh] generation={} phase=execute thread={}", a_generation, GetCurrentThreadId());
    }
    bool current;
    {
        std::lock_guard lock(_mutex);
        current = a_generation == _generation && _sessionActive;
    }
    if (!current) {
        complete({}, false, "stale-generation");
        return;
    }
    auto* ui = RE::UI::GetSingleton();
    if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
        Reset();
        complete({}, false, "map-closed");
        return;
    }

    std::vector<ActiveQuestRow> rows;

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!player || !dh) {
        complete({}, true, !player ? "missing-player" : "missing-data-handler");
        return;
    }
    const auto markers = HUDMarkerHook::GetQuestMarkers();
    auto* playerWorld = player->GetWorldspace();

    std::vector<std::string> logBlock;
    g_logBlock = qlog ? &logBlock : nullptr;

    for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
        if (!quest || !quest->IsRunning()) continue;

        // Skip miscellaneous quests - they are collected via the player objectives path.
        if (quest->GetType() == RE::QUEST_DATA::Type::kMiscellaneous) continue;

        if (!quest->data.flags.any(RE::QuestFlag::kActive)) continue;
        if (!quest->data.flags.any(RE::QuestFlag::kDisplayedInHUD)) continue;

        const char* qname = quest->GetFullName();
        if (!qname || !qname[0]) continue;

        ActiveQuestRow row;
        row.questID   = quest->GetFormID();
        row.name      = qname;
        row.questType = static_cast<std::int32_t>(quest->GetType());

        RE::BGSQuestObjective* firstObj = nullptr;
        for (auto* obj : quest->objectives) {
            if (!obj) continue;
            if (obj->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed) continue;
            firstObj = obj;
            row.objectiveText = FormatObjectiveText(obj, quest, quest->currentInstanceID);
            break;
        }

        const char* eligibilityReason = nullptr;
        const auto eligibleTargets = CopyEligibleTargets(player, quest, eligibilityReason);
        ResolveObjectivePosition(row, quest, firstObj, markers, playerWorld, true, eligibleTargets, eligibilityReason);
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
                if (qlog) QLog("[QuestMisc] quest=00000000 instance={} currentInstance=0 objective=0 state={} active=0 master=1 kept=0 reason=null-objective",
                                       entry.instanceID, static_cast<std::int32_t>(entry.InstanceState));
                continue;
            }
            if (!quest) {
                if (qlog) QLog("[QuestMisc] quest=00000000 instance={} currentInstance=0 objective={} state={} active=0 master=1 kept=0 reason=null-quest",
                                       entry.instanceID, objective->index, static_cast<std::int32_t>(entry.InstanceState));
                continue;
            }
            if (quest->GetType() != RE::QUEST_DATA::Type::kMiscellaneous) {
                if (qlog) QLog("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=non-misc",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }
            if (entry.InstanceState != RE::QUEST_OBJECTIVE_STATE::kDisplayed) {
                if (qlog) QLog("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=not-displayed",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }
            if (!quest->data.flags.any(RE::QuestFlag::kActive)) {
                if (qlog) QLog("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active=0 master=1 kept=0 reason=inactive",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState));
                continue;
            }
            if (quest->currentInstanceID != entry.instanceID) {
                if (qlog) QLog("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=instance-mismatch",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }

            std::string text = FormatObjectiveText(objective, quest, entry.instanceID);
            if (text.empty()) {
                if (qlog) QLog("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=empty-text",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }

            const MiscKey key{ quest->GetFormID(), entry.instanceID, objective->index };
            if (!seen.insert(key).second) {
                if (qlog) QLog("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active={} master=1 kept=0 reason=duplicate",
                                       quest->GetFormID(), entry.instanceID, quest->currentInstanceID, objective->index,
                                       static_cast<std::int32_t>(entry.InstanceState), quest->data.flags.any(RE::QuestFlag::kActive));
                continue;
            }

            if (qlog) {
                QLog("[QuestMisc] quest={:08X} instance={} currentInstance={} objective={} state={} active=1 master=1 kept=1 reason=accepted",
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

            const char* eligibilityReason = nullptr;
            const auto eligibleTargets = CopyEligibleTargets(player, quest, eligibilityReason);
            ResolveObjectivePosition(row, quest, objective, markers, playerWorld, false, eligibleTargets, eligibilityReason);
            rows.push_back(std::move(row));
        }
    } else if (player && !masterOn && qlog) {
        QLog("[QuestMisc] master=0 kept=0 reason=master-off");
    }

    if (qlog) {
        for (const auto& r : rows) {
            const char* kind = r.isMiscellaneous ? "misc" : "regular";
            const std::string& text = r.isMiscellaneous ? r.objectiveText : r.name;
            QLog("[QuestRow] {:08X} \"{}\" kind={} instance={} objective={} hasPos={} world={:08X} src={} pos=({:.1f},{:.1f},{:.1f})",
                         r.questID, text, kind, r.instanceID, r.objectiveIndex,
                         r.hasPosition, r.worldSpaceID, r.posSource,
                         r.position.x, r.position.y, r.position.z);
        }
    }

    g_logBlock = nullptr;
    if (qlog) {
        static std::vector<std::string> lastBlock;
        if (logBlock != lastBlock) {
            logger::info("[QuestLog] generation={} lines={} (block changed; identical refreshes are not logged)",
                         a_generation, logBlock.size());
            for (const auto& line : logBlock) {
                logger::info("{}", line);
            }
            lastBlock = std::move(logBlock);
        }
    }

    complete(std::move(rows), true, "collected");
}

std::vector<ActiveQuestRow> QuestTracker::Get() {
    std::lock_guard lock(_mutex);
    return _rows;
}
