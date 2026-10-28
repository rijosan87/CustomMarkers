// Helper to manage fonts and localization glyphs
#pragma once

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

    // Load fonts based on current settings
    void LoadFonts(ImGuiIO& io);

    // Get pointers to loaded fonts
    ImFont* GetBaseFont() const { return baseFont; }
    ImFont* GetHeadingFont() const { return headingFont; }

    // Helper to get glyph ranges based on language code
    const ImWchar* GetGlyphRanges(const std::string& language, ImGuiIO& io);

private:
    FontManager() = default;
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    std::string GetFontPath(const std::string& language);

    ImFont* baseFont = nullptr;
    ImFont* headingFont = nullptr;
};
