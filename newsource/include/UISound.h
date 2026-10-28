#pragma once

namespace UISound {
    inline void PlayMapMarkerHover() {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            return;
        }

        // Hover detection runs from the Present hook. Skyrim audio calls belong
        // on the game thread, so defer the cue through SKSE's task queue.
        tasks->AddTask([]() {
            auto* audio = RE::BSAudioManager::GetSingleton();
            if (!audio) {
                return;
            }

            // UIMapRolloverFlyout, Skyrim.esm SNDR 000C0DF3. CommonLibSSE-NG
            // 7.x resolves the descriptor and sound handle inside Play(FormID).
            audio->Play(static_cast<RE::FormID>(0x000C0DF3));
        });
    }
}
