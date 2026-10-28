#include "FontManager.h"
#include "Settings.h"
#include <imgui_freetype.h>
#include <Windows.h>

// Custom glyph ranges for European languages (Latin + Polish + Cyrillic)
static const ImWchar kEuropeanRanges[] = {
    0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement (German: ä, ö, ü, ß)
    0x0100, 0x017F,  // Latin Extended-A (Polish: ą, ć, ę, ł, ń, ó, ś, ź, ż)
    0x0400, 0x052F,  // Cyrillic (Russian, Ukrainian, etc.)
    0,               // Null terminator
};

void FontManager::LoadFonts(ImGuiIO& io) {
    auto settings = Settings::GetSingleton();
    std::string fontPath = GetFontPath(settings->language);

    ImFontConfig fontConfig;
    fontConfig.PixelSnapH = true;

    // Horizontal oversampling sharpens glyphs drawn at fractional X positions
    // (centered/right-aligned text, scaled layouts) — keep it even with FreeType.
    // Vertical oversampling rarely matters, so 1 there to save atlas height.
    bool isCJK = settings->language.find("_zh") != std::string::npos ||
                    settings->language.find("_ja") != std::string::npos ||
                    settings->language.find("_ko") != std::string::npos;

    fontConfig.OversampleH = isCJK ? 2 : 3;
    fontConfig.OversampleV = 2;

    // No hinting: hinting snaps glyph edges to the pixel grid, which sharpens stems
    // but increases the hard-edge aliasing visible at 100% (1:1) over the game scene.
    // Unhinted keeps FreeType's coverage AA on the edges instead — smoother for a HUD.
    fontConfig.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_NoHinting;

    float baseFontSize = static_cast<float>(settings->fontSize);
    float headingSize = static_cast<float>(settings->headingFontSize);
    const ImWchar* glyphRanges = GetGlyphRanges(settings->language, io);

    // Reset fonts
    baseFont = nullptr;
    headingFont = nullptr;
    io.Fonts->Clear(); // Clear existing fonts before reloading

    // Use the FreeType rasterizer instead of the default stb_truetype. stb has no
    // hinting and weaker AA, which is what made our text look soft at native size
    // next to Scaleform. FreeType is linked via vcpkg imgui[freetype] but is NOT
    // the default — it has to be selected here, before Build().
    io.Fonts->FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();

    if (!fontPath.empty()) {
        logger::info("Loading base font from: {} at {}px", fontPath, baseFontSize);
        baseFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), baseFontSize, &fontConfig, glyphRanges);

        // Load heading font (Latin only - skip for CJK to save memory)
        if (!isCJK && headingSize > baseFontSize) {
            logger::info("Loading heading font at {}px", headingSize);
            headingFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), headingSize, &fontConfig, glyphRanges);
        }
    }

    if (baseFont) {
        logger::info("Base font loaded successfully at {}px", baseFontSize);
    } else {
        logger::warn("Could not load font from {}, using ImGui default font", fontPath.empty() ? "(none)" : fontPath);
        io.Fonts->AddFontDefault();
        baseFont = io.Fonts->Fonts[0]; // Set baseFont to default if loading failed
    }

    // Build atlas
    io.Fonts->Build();
}

const ImWchar* FontManager::GetGlyphRanges(const std::string& language, ImGuiIO& io) {
    // CJK language detection
    if (language.find("_zh") != std::string::npos) {
        return io.Fonts->GetGlyphRangesChineseFull();
    }
    if (language.find("_ja") != std::string::npos) {
        return io.Fonts->GetGlyphRangesJapanese();
    }
    if (language.find("_ko") != std::string::npos) {
        return io.Fonts->GetGlyphRangesKorean();
    }
    if (language.find("_vi") != std::string::npos) {
        return io.Fonts->GetGlyphRangesVietnamese();
    }
    if (language.find("_th") != std::string::npos) {
        return io.Fonts->GetGlyphRangesThai();
    }
    if (language.find("_el") != std::string::npos) {
        return io.Fonts->GetGlyphRangesGreek();
    }
    // Default: European languages (Latin + Polish + Cyrillic)
    return kEuropeanRanges;
}

std::string FontManager::GetFontPath(const std::string& language) {
    // 1. Try language-specific font in Data/SKSE/Plugins/fonts/
    std::string customPath = "Data/SKSE/Plugins/fonts/" + language + ".ttf";
    if (std::filesystem::exists(customPath)) {
        return customPath;
    }

    // 2. Fallback to Segoe UI (Windows System Font)
    std::string segoeUIPath = "C:\\Windows\\Fonts\\seguisb.ttf";
    if (std::filesystem::exists(segoeUIPath)) {
        return segoeUIPath;
    }

    // 3. Fallback to failure (will trigger ImGui default)
    return "";
}
