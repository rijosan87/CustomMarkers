// include/LookPinRenderer.h
#pragma once
#include <RE/Skyrim.h>

// Look & Pin: toggle mode that places a custom marker where the player is
// looking. Exterior only (spec decision 6: an interior marker stores
// worldSpaceID 0 and would never satisfy WorldBeaconRenderer's exterior space
// check, so the beacon would silently never draw).
//
// IsActive()/ForceExit() are static because InputHook calls them from the input
// thread. They go through a file-local atomic, NOT the _active member.
class LookPinRenderer {
public:
    static LookPinRenderer* GetSingleton() {
        static LookPinRenderer singleton;
        return &singleton;
    }

    void Draw();

    // InputHook queries this to decide whether to swallow the toggle key and ESC.
    static bool IsActive();
    static void ForceExit();

private:
    bool _active       = false;
    bool _keyWasDown   = false;
    bool _hasTarget    = false;
    bool _targetIsLand = false;   // hit came from the GetLandHeight fallback (log only)

    RE::NiPoint3 _targetPos{};

    float  _fade             = 0.0f;  // 0..1 ease so the reticle doesn't pop
    double _noTargetMsgUntil = 0.0;   // show "No target" until this timestamp

    // Last good hit, used to coast over a one-frame ray dropout at the edge of
    // loaded collision. Only reused while recent AND the aim has barely moved.
    RE::NiPoint3 _lastHitPos{};
    RE::NiPoint3 _lastHitFwd{};
    double _lastHitTime    = 0.0;
    bool   _lastHitValid   = false;
    bool   _lastHitWasLand = false;
};
