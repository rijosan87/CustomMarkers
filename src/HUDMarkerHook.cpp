// Hooks in-game HUD marker rendering to display custom destination pins.

#include "HUDMarkerHook.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <polyhook2/Detour/x64Detour.hpp>

// STATUS (update when behavior changes; do NOT mirror into CLAUDE.md):

namespace {
    constexpr REL::RelocationID kAddMarkerID{ 50851, 51728 };

    struct FrameOffsets {
        std::uint32_t quest;
        std::uint32_t questDoor;
        std::uint32_t playerSet;
        std::uint32_t enemy;
        std::uint32_t location;
        std::uint32_t undiscoveredLocation;
    };
    static_assert(sizeof(FrameOffsets) == 0x18);

    FrameOffsets* GetFrameOffsets() {
        REL::Relocation<FrameOffsets*> singleton{ REL::RelocationID(519587, 406118) };
        return singleton.get();
    }

    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    using AddMarkerFn = void(*)(const void*, void*, void*, const std::uint32_t*, std::int32_t);

    std::mutex                                g_mutex;
    std::vector<HUDMarkerHook::QuestMarker>   g_pending;
    std::vector<HUDMarkerHook::QuestMarker>   g_published;

    std::unordered_map<std::uint32_t, RE::TESQuest*> g_handleToQuest;
    std::chrono::steady_clock::time_point            g_lastAliasRebuild{};

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
                auto [it, inserted] = g_handleToQuest.try_emplace(handle, quest);
                if (!inserted && IsJournalVisible(quest) && !IsJournalVisible(it->second)) {
                    it->second = quest;
                }
            }
        }
    }

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
        if (IsQuestFrame(a_gotoFrame) && a_refHandle) {
            if (auto refPtr = RE::TESObjectREFR::LookupByHandle(*a_refHandle)) {
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
        {
            std::lock_guard lock(g_mutex);
            g_published.swap(g_pending);
            g_pending.clear();
        }
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
