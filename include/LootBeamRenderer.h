#pragma once

// Renders 3D light beams over valuable loot, corpses, and containers.


#include <RE/Skyrim.h>
#include <imgui.h>

class LootBeamRenderer {
public:
    static LootBeamRenderer* GetSingleton() {
        static LootBeamRenderer singleton;
        return &singleton;
    }
    void Draw();
};
