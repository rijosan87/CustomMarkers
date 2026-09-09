#pragma once

// Raycasts crosshair position to place custom markers directly in the 3D world.

#include <RE/Skyrim.h>

class LookPinRenderer {
public:
    static LookPinRenderer* GetSingleton() {
        static LookPinRenderer singleton;
        return &singleton;
    }
    void Draw();
    static bool IsActive();
    static void ForceExit();
private:
    bool _active       = false;
    bool _keyWasDown   = false;
    bool _hasTarget    = false;
    bool _targetIsLand = false;
    RE::NiPoint3 _targetPos{};
    float  _fade             = 0.0f;
    double _noTargetMsgUntil = 0.0;
    RE::NiPoint3 _lastHitPos{};
    RE::NiPoint3 _lastHitFwd{};
    double _lastHitTime    = 0.0;
    bool   _lastHitValid   = false;
    bool   _lastHitWasLand = false;
};
