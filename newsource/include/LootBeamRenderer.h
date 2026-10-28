#pragma once

#include <RE/Skyrim.h>
#include <imgui.h>
#include <atomic>

class LootBeamRenderer : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
public:
    static LootBeamRenderer* GetSingleton() {
        static LootBeamRenderer singleton;
        return &singleton;
    }

    void InstallEventSink();
    void ClearLootedHolders();
    void Draw();

    static bool IsVisible() { return GetSingleton()->_visible.load(); }
    static void ToggleVisible() {
        auto* inst = GetSingleton();
        inst->_visible.store(!inst->_visible.load());
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESContainerChangedEvent* a_event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*) override;

private:
    bool _eventSinkInstalled = false;
    std::atomic<bool> _visible{true};  // session-only hotkey state
};
