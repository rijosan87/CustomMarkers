#include "PCH.h"

#include "CompassVisibility.h"
#include "Settings.h"

#include <chrono>

namespace CompassVisibility {
    namespace {
        constexpr float kApplySeconds = 1.0f;
        constexpr const char* kCompassPath = "_root.HUDMovieBaseInstance.CompassShoutMeterHolder.Compass";
        constexpr const char* kQuestListPath = "_root.HUDMovieBaseInstance.QuestItemList";

        std::atomic_bool g_taskPending{ false };
        std::atomic_bool g_compassHidden{ false };
        std::atomic_bool g_listHidden{ false };

        void Apply() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME);
            if (!hud || !hud->uiMovie) return;
            const auto* settings = Settings::GetSingleton();

            if (settings->navHideCompass || g_compassHidden) {
                RE::GFxValue clip;
                if (hud->uiMovie->GetVariable(&clip, kCompassPath) && clip.IsDisplayObject()) {
                    RE::GFxValue alpha;
                    alpha.SetNumber(settings->navHideCompass ? 0.0 : 100.0);
                    clip.SetMember("_alpha", alpha);
                }
                g_compassHidden = settings->navHideCompass;
            }

            if (settings->navHideCNOQuestList || g_listHidden) {
                RE::GFxValue clip;
                if (hud->uiMovie->GetVariable(&clip, kQuestListPath) && clip.IsDisplayObject()) {
                    RE::GFxValue scale;
                    scale.SetNumber(settings->navHideCNOQuestList ? 0.0 : 100.0);
                    clip.SetMember("_xscale", scale);
                    clip.SetMember("_yscale", scale);
                }
                g_listHidden = settings->navHideCNOQuestList;
            }
        }
    }

    void RequestUpdateIfDue() {
        const auto* settings = Settings::GetSingleton();
        if (!settings->navHideCompass && !settings->navHideCNOQuestList && !g_compassHidden && !g_listHidden) return;

        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - last).count() < kApplySeconds) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks || g_taskPending.exchange(true)) return;
        last = now;
        tasks->AddUITask([]() {
            Apply();
            g_taskPending.store(false);
        });
    }
}