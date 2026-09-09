#pragma once

// Renders local and interior map custom pins and breadcrumb trails.

#include <atomic>
#include <cstdint>
#include <unordered_map>

class LocalMapRenderer {
public:
    static LocalMapRenderer* GetSingleton() {
        static LocalMapRenderer singleton;
        return &singleton;
    }
    void Install();
    void Draw();
    std::uint32_t GetRemoveButtonTarget() const { return _removeButtonTarget.load(); }
    std::uint32_t GetDestButtonTarget()   const { return _destButtonTarget.load(); }
    std::uint32_t GetEditButtonTarget()   const { return _editButtonTarget.load(); }
    bool IsCustomPinHovered() const { return _activeMarkerId != 0; }
    std::uint32_t GetHoveredMarkerId() const { return _activeMarkerId; }
    bool IsBeaconHovered() const { return _beaconHovered.load(); }
private:
    LocalMapRenderer() = default;
    ~LocalMapRenderer() = default;
    LocalMapRenderer(const LocalMapRenderer&) = delete;
    LocalMapRenderer& operator=(const LocalMapRenderer&) = delete;
    std::uint32_t              _activeMarkerId = 0;
    std::atomic<std::uint32_t> _removeButtonTarget{ 0 };
    std::atomic<std::uint32_t> _destButtonTarget{ 0 };
    std::atomic<std::uint32_t> _editButtonTarget{ 0 };
    std::atomic<bool>          _beaconHovered{ false };
    std::unordered_map<std::uint32_t, float> _hoverAnim;
};
