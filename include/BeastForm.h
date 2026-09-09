#pragma once

// Detects werewolf and vampire lord beast form states.


#include <RE/Skyrim.h>

namespace BeastForm {
    inline bool IsBeastForm() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        auto* race = player->GetRace();
        if (!race) return false;
        if (race->GetFormID() == 0x000CDD84) return true;
        static RE::TESRace* s_werebearRace = nullptr;
        if (!s_werebearRace) {
            s_werebearRace = RE::TESForm::LookupByEditorID<RE::TESRace>("DLC2WerebearBeastRace");
        }
        if (s_werebearRace && race == s_werebearRace) return true;
        return false;
    }
}
