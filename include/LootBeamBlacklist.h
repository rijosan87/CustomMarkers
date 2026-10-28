#pragma once
#include <RE/Skyrim.h>

namespace LootBeamBlacklist {
    void Load();
    bool IsEnvironmentBlacklisted(const RE::TESObjectCELL* a_cell);
    bool IsReferenceBlacklisted(const RE::TESObjectREFR& a_ref,
                                bool* a_baseMatch = nullptr);
}
