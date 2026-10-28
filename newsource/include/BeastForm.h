// include/BeastForm.h
// Player beast-form detection for gameplay-view overlay gating.
//
// Ported from Party Sheet's PlayerStatus::GetTransformState (src/PlayerStatus.cpp),
// which is a working, shipped implementation — worth copying verbatim in shape
// rather than re-deriving, because Werebear in particular is easy to get wrong.
//
// WHY ONLY THE BEAST FORMS:
//   Werewolf and Werebear strip the player's whole UI relationship with the
//   world: no inventory, no map, no fast travel, no equipment, and crucially
//   NO LOOTING. Loot beams would point at containers the player physically
//   cannot open. The radar, beacons and Look & Pin are the same story — a
//   timed rage form is not a navigation state.
//
//   Vampire Lord is deliberately NOT included. It keeps inventory, spells, the
//   map and fast travel, has no timer, and reverts at will — a controlled
//   combat form where navigation is a normal thing to do. Do not "fix" this by
//   adding it; the omission is the design.
//
// WHY EDITORID FOR THE DLC RACES:
//   Werewolf is Skyrim.esm and always present, so its FormID is safe to hardcode.
//   Vampire Lord / Werebear come from DLC, where a hardcoded FormID breaks under
//   a different load order. The EditorID lookup is load-order independent and
//   safe when the DLC is absent. It also picks up mods that reuse the Werebear
//   race (Berserkyr and similar) for free.
#pragma once

#include <RE/Skyrim.h>

namespace BeastForm {

    // True while the player is a Werewolf or Werebear. Vampire Lord returns
    // false on purpose (see header comment).
    inline bool IsBeastForm() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        auto* race = player->GetRace();
        if (!race) return false;

        // Werewolf race — Skyrim.esm 0x000CDD84 (always present).
        if (race->GetFormID() == 0x000CDD84) return true;

        // Werebear race — Dragonborn DLC. Cached statics retry until the DLC
        // forms are available; the first call can land before they load.
        static RE::TESRace* s_werebearRace = nullptr;
        if (!s_werebearRace) {
            s_werebearRace = RE::TESForm::LookupByEditorID<RE::TESRace>("DLC2WerebearBeastRace");
        }
        if (s_werebearRace && race == s_werebearRace) return true;

        return false;
    }

}  // namespace BeastForm
