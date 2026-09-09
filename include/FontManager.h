#pragma once

// Handles custom ImGui font loading and fallback configuration.


#include <imgui.h>
#include <string>
#include <vector>
#include <filesystem>

class FontManager {
public:
    static FontManager* GetSingleton() {
        static FontManager singleton;
        return &singleton;
    }
    void LoadFonts(ImGuiIO& io);
    ImFont* GetBaseFont() const { return baseFont; }
    ImFont* GetHeadingFont() const { return headingFont; }
    const ImWchar* GetGlyphRanges(const std::string& language, ImGuiIO& io);
private:
    FontManager() = default;
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;
    std::string GetFontPath(const std::string& language);
    ImFont* baseFont = nullptr;
    ImFont* headingFont = nullptr;
};
