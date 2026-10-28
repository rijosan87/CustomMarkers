#include "PCH.h"
#include "TCCItemStatus.h"

#include <SKSE/SKSE.h>

namespace TCCItemStatus {
    namespace {
        constexpr auto kPlugin = "DBM_RelicNotifications.esp";
        RE::BGSListForm* g_new = nullptr;
        RE::BGSListForm* g_found = nullptr;
        RE::BGSListForm* g_displayed = nullptr;
        RE::TESGlobal* g_setupStart = nullptr;
        RE::TESGlobal* g_setupFinish = nullptr;
    }

    void Init() {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data || (!data->LookupLoadedModByName(kPlugin) &&
                      !data->LookupLoadedLightModByName(kPlugin))) {
            SKSE::log::info("TCC item status unavailable: {} is not loaded", kPlugin);
            return;
        }

        g_new = data->LookupForm<RE::BGSListForm>(0x558285, kPlugin);
        g_found = data->LookupForm<RE::BGSListForm>(0x558286, kPlugin);
        g_displayed = data->LookupForm<RE::BGSListForm>(0x558287, kPlugin);
        g_setupStart = data->LookupForm<RE::TESGlobal>(0x1DCE24, kPlugin);
        g_setupFinish = data->LookupForm<RE::TESGlobal>(0x1DCE25, kPlugin);
        if (!g_new || !g_found || !g_displayed || !g_setupStart || !g_setupFinish) {
            g_new = g_found = g_displayed = nullptr;
            g_setupStart = g_setupFinish = nullptr;
            SKSE::log::warn("TCC item status unavailable: status lists or setup globals are missing");
            return;
        }
        SKSE::log::info("TCC item status lists found");
    }

    Status Get(RE::FormID a_itemFormID) {
        // TCC sets Finish only after it populates the lists and clears Start.
        // A loaded plugin alone does not mean its Papyrus state is ready.
        if (!a_itemFormID || !g_new || g_setupStart->value != 0.0f ||
            g_setupFinish->value == 0.0f) return Status::None;
        // TCC moves items between these lists. During a script update, prefer
        // the furthest progressed state if two memberships briefly overlap.
        if (g_displayed->HasForm(a_itemFormID)) return Status::Displayed;
        if (g_found->HasForm(a_itemFormID)) return Status::Found;
        if (g_new->HasForm(a_itemFormID)) return Status::New;
        return Status::None;
    }

    const char* Name(Status a_status) {
        switch (a_status) {
            case Status::New: return "New";
            case Status::Found: return "Found";
            case Status::Displayed: return "Displayed";
            default: return "None";
        }
    }
}
