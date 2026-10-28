#pragma once
#include <filesystem>
#include <string>

namespace PathUtils {
    inline std::string ToUtf8(const std::filesystem::path& a_path) {
        const auto u8 = a_path.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
}
