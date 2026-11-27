#pragma once

#include <cstdint>
#include <string>

namespace PNGIIBridge {
    void Init();

    void RequestFill();

    const std::string* MapIconPath(std::uint32_t a_iconType, bool a_local);

    bool CanReadFiles();

    bool ReadPackFile(const std::string& a_dataPath, std::string& a_out);

    bool ResolveRefIcon(std::uint32_t a_formID, std::uint32_t a_locationID, std::uint16_t a_type,
                        bool a_undiscovered, std::string& a_out);

    inline constexpr std::uint32_t kHasColor = 0x01000000;

    std::uint32_t ColorGeneration();

    bool ResolveRefColor(std::uint32_t a_formID, std::uint32_t a_locationID, std::uint16_t a_type,
                         bool a_undiscovered, std::uint32_t& a_rgb);
}