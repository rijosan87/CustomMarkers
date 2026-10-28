#pragma once

#include <RE/Skyrim.h>
#include <imgui.h>

class WorldBeaconRenderer {
public:
    static WorldBeaconRenderer* GetSingleton() {
        static WorldBeaconRenderer singleton;
        return &singleton;
    }

    void Draw();

private:
    float _beaconFade = 0.0f;
};
