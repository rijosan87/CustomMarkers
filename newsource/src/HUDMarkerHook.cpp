#include "HUDMarkerHook.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <polyhook2/Detour/x64Detour.hpp>

// ============================================================================
// STATUS (update when behavior changes; do NOT mirror into CLAUDE.md):
//   - Entry-detours the engine's AddMarker, RELOCATION_ID(50851, 51728). Signature
//     RE'd from MiniMap Menu (FUN_18001d9c0) + CNO. It is VOID, 5 params:
//       void AddMarker(HUDMarkerManager*, void*, void* param3,
//                      RefHandle* param4, int param5)
//     Declaring it bool corrupts the return path (that was the original bug).
//     The engine calls it once per resolved compass marker per frame.
//   - param5 = marker type: 1 quest, 2 questDoor, 3 playerSet, 4 enemy, 5 location.
//     We keep 1 and 2 (IsQuestFrame checks HUDMarker::FrameOffsets quest/questDoor,
//     RELOCATION_ID(519587, 406118), which equal 1/2 here).
//   - WORLD POSITION is read from the RESOLVED REF, not any AddMarker arg: the
//     engine's pos arg is compass/screen space (reads 0,0,0). param4 is a RefHandle*;
//     LookupByHandle(*param4)->GetPosition() gives the true world pos, live for NPCs.
//     Same source MiniMap Menu (ref +0x54) and CNO use. CONFIRMED IN-GAME 2026-07-20.
//   - Buffer per pass, publish in EndPass() called at the real Compass::Update
//     boundary (CompassHook trampoline return, all paths). No index heuristic.
//   - Positions are the engine's already tracked-filtered + door-retargeted set,
//     the exact markers vanilla's compass shows. No quest walking, no kActive.
//     See findings-MiniMapMenu-QuestMarkers.md + findings-HUDMarker-Pipeline-QuestMarkers.md.
//   - AE 1.6.1170 primary + confirmed. SE ids are the RELOCATION_ID first element
//     (50851 / 519587), untested on SE; verify before shipping SE.
// ============================================================================

namespace {
    // AddMarker: SE 50851 / AE 51728.
    constexpr REL::RelocationID kAddMarkerID{ 50851, 51728 };

    // HUDMarker::FrameOffsets singleton (icon goto-frame table). SE 519587 / AE 406118.
    struct FrameOffsets {
        std::uint32_t quest;                 // 00
        std::uint32_t questDoor;             // 04
        std::uint32_t playerSet;             // 08
        std::uint32_t enemy;                 // 0C
        std::uint32_t location;              // 10
        std::uint32_t undiscoveredLocation;  // 14
    };
    static_assert(sizeof(FrameOffsets) == 0x18);

    FrameOffsets* GetFrameOffsets() {
        REL::Relocation<FrameOffsets*> singleton{ REL::RelocationID(519587, 406118) };
        return singleton.get();
    }

    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    // AddMarker is VOID (RE of MiniMap Menu FUN_18001d9c0, findings-MiniMapMenu-
    // QuestMarkers.md): void(HUDMarkerManager*, void*, void*, RefHandle* param_4, int param_5).
    // param_4 (r9) = pointer to the RefHandle (uint32); param_5 (stack) = marker
    // type: 1 quest, 2 questDoor, 3 playerSet, 4 enemy. The 3rd param is opaque
    // here, we resolve the ref from param_4 and read GetPosition(). Declaring it
    // bool corrupts the return path.
    using AddMarkerFn = void(*)(const void*, void*, void*, const std::uint32_t*, std::int32_t);

    std::mutex                                g_mutex;
    std::vector<HUDMarkerHook::QuestMarker>   g_pending;    // accumulating the current pass
    std::vector<HUDMarkerHook::QuestMarker>   g_published;  // last completed pass, read by renderers

    // Marker ref -> owning quest, via the QUEST-side alias map.
    //
    // The obvious path (ExtraFromAlias on the ref) is DEAD: probed in-game
    // 2026-08-19, all 8 tracked markers returned no alias, NPC quest targets
    // included -- not just doors. That extra is stamped on only a few refs, it
    // is not general alias membership. Aliases live quest-side in
    // TESQuest::refAliasMap (+0x070), a BSTHashMap<aliasID, ObjectRefHandle>.
    //
    // So invert it: walk every running quest once, read its refAliasMap, and
    // build handle -> quest. Rebuilt on demand rather than per marker; the walk
    // is over quests (tens), not refs. refAliasMap is guarded by the quest's own
    // aliasAccessLock, so take a read lock per quest.
    std::unordered_map<std::uint32_t, RE::TESQuest*> g_handleToQuest;
    std::chrono::steady_clock::time_point            g_lastAliasRebuild{};

    // kDisplayedInHUD is the journal's own filter: quests the player actually
    // sees listed. Helper/utility quests (followers, frameworks) run without it.
    bool IsJournalVisible(const RE::TESQuest* a_quest) {
        return a_quest && a_quest->data.flags.any(RE::QuestFlag::kDisplayedInHUD);
    }

    void RebuildHandleToQuestMap() {
        g_handleToQuest.clear();

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;

        for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
            if (!quest || !quest->IsRunning()) continue;

            RE::BSReadLockGuard lock(quest->aliasAccessLock);
            for (const auto& entry : quest->refAliasMap) {
                const std::uint32_t handle = entry.second.native_handle();
                if (!handle) continue;

                // One ref is routinely owned by SEVERAL quests, so first-writer-
                // wins picks the wrong one. Measured 2026-08-19: Melana's ref
                // resolved to DZ08_MelanaFollower (a nameless helper quest)
                // while the journal shows DZ08_MelanaIntro / "Maiden in the
                // Tower". The name lookup was never broken -- the QUEST choice
                // was. Prefer the journal-visible quest, the same set the
                // journal itself lists.
                auto [it, inserted] = g_handleToQuest.try_emplace(handle, quest);
                if (!inserted && IsJournalVisible(quest) && !IsJournalVisible(it->second)) {
                    it->second = quest;
                }
            }
        }
    }

    // 0 when the handle belongs to no quest alias. Common and not an error:
    // questDoor markers are engine-retargeted load doors carrying no alias.
    RE::FormID ResolveQuestFromHandle(std::uint32_t a_handle) {
        const auto it = g_handleToQuest.find(a_handle);
        if (it == g_handleToQuest.end() || !it->second) return 0;
        return it->second->GetFormID();
    }

    bool IsQuestFrame(std::int32_t gotoFrame) {
        auto* fo = GetFrameOffsets();
        if (!fo) return false;
        return gotoFrame == static_cast<std::int32_t>(fo->quest) ||
               gotoFrame == static_cast<std::int32_t>(fo->questDoor);
    }

    void HookedAddMarker(const void* a_mgr, void* a_markerData, void* a_param3,
                         const std::uint32_t* a_refHandle, std::int32_t a_gotoFrame) {
        // Accumulate every quest/questDoor marker of the current compass pass;
        // EndPass() publishes at the real Compass::Update boundary. World position
        // comes from the RESOLVED ref (LookupByHandle -> GetPosition), NOT from any
        // AddMarker argument (the engine's pos arg is compass/screen space, reads
        // 0,0,0 here). Same source MiniMap Menu and CNO use.
        if (IsQuestFrame(a_gotoFrame) && a_refHandle) {
            if (auto refPtr = RE::TESObjectREFR::LookupByHandle(*a_refHandle)) {
                // Quest identity resolves in the SAME branch as the position, so
                // the two can never desync: a ref that fails to resolve
                // contributes neither.
                HUDMarkerHook::QuestMarker entry;
                entry.position = refPtr->GetPosition();
                entry.questID  = ResolveQuestFromHandle(*a_refHandle);

                std::lock_guard lock(g_mutex);
                g_pending.push_back(entry);
            }
        }
        reinterpret_cast<AddMarkerFn>(g_trampolineAddr)(a_mgr, a_markerData, a_param3, a_refHandle, a_gotoFrame);
    }
}

namespace HUDMarkerHook {
    void Install() {
        REL::Relocation<std::uintptr_t> target{ kAddMarkerID };
        const std::uint64_t targetAddr = target.address();
        const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedAddMarker);

        g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

        if (!g_detour->hook()) {
            logger::error("HUDMarkerHook: PolyHook x64Detour::hook() returned false");
            g_detour.reset();
            return;
        }

        logger::info("HUDMarkerHook installed | target={:X} | trampoline={:X}",
                     targetAddr, g_trampolineAddr);
    }

    void EndPass() {
        // Called at the end of each Compass::Update pass (from CompassHook). Swap
        // the accumulated quest markers into the published buffer and reset. This
        // is the real frame boundary; AddMarker only appends between EndPass calls.
        {
            std::lock_guard lock(g_mutex);
            g_published.swap(g_pending);
            g_pending.clear();
        }

        // Rebuild the alias map for the NEXT pass, throttled to 5s. EndPass runs
        // once per compass update (per frame), and the walk touches every running
        // quest's refAliasMap under that quest's own read lock, so rebuilding it
        // every frame would be the expensive part of this hook. Aliases only
        // change on quest state changes, which is far slower than 5s.
        //
        // Deliberately OUTSIDE g_mutex: that mutex guards the published buffer for
        // the Present-thread readers, and holding it across a full quest walk
        // would stall them. g_handleToQuest is only ever touched here and in
        // HookedAddMarker, both on the game main thread.
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastAliasRebuild >= std::chrono::seconds(5)) {
            g_lastAliasRebuild = now;
            RebuildHandleToQuestMap();
        }
    }

    std::vector<RE::NiPoint3> GetQuestMarkerPositions() {
        std::lock_guard lock(g_mutex);
        std::vector<RE::NiPoint3> out;
        out.reserve(g_published.size());
        for (const auto& m : g_published) out.push_back(m.position);
        return out;
    }

    std::vector<QuestMarker> GetQuestMarkers() {
        std::lock_guard lock(g_mutex);
        return g_published;
    }
}
