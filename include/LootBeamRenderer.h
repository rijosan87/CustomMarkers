#pragma once

#include <RE/Skyrim.h>
#include <SKSE/Events.h>
#include <imgui.h>
#include <atomic>
#include <cstdint>

class LootBeamRenderer : public RE::BSTEventSink<RE::TESContainerChangedEvent>,
                         public RE::BSTEventSink<RE::TESDeathEvent>,
                         public RE::BSTEventSink<SKSE::CrosshairRefEvent> {
public:
    static LootBeamRenderer* GetSingleton() {
        static LootBeamRenderer singleton;
        return &singleton;
    }

    void InstallEventSink();
    void ClearLootedHolders();
    void ClearInspectedHolders();
    void MarkInspectedHolder();
    void ClearFreshKillDelays();
    void ClearActivationFade();
    void Draw();

    static bool IsVisible() { return GetSingleton()->_visible.load(); }
    static void ToggleVisible() {
        auto* inst = GetSingleton();
        inst->_visible.store(!inst->_visible.load());
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESContainerChangedEvent* a_event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*) override;

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESDeathEvent* a_event,
        RE::BSTEventSource<RE::TESDeathEvent>*) override;

    RE::BSEventNotifyControl ProcessEvent(
        const SKSE::CrosshairRefEvent* a_event,
        RE::BSTEventSource<SKSE::CrosshairRefEvent>*) override;

private:
    bool _eventSinkInstalled = false;
    std::atomic<bool> _visible{true};
    std::atomic<RE::FormID> _crosshairTarget{0};
    std::atomic<std::uint32_t> _activationFadeReset{0};
};
