#pragma once

// Plays UI sound effects through Skyrim's audio system.


namespace UISound {
    inline void PlayMapMarkerHover() {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            return;
        }
        tasks->AddTask([]() {
            auto* audio = RE::BSAudioManager::GetSingleton();
            if (!audio) {
                return;
            }
            audio->Play(static_cast<RE::FormID>(0x000C0DF3));
        });
    }
}
