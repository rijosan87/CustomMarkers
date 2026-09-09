// Handles custom ImGui font loading and fallback configuration.

#include "FontManager.h"
#include "Settings.h"
#include <imgui_freetype.h>
#include <Windows.h>

static const ImWchar kEuropeanRanges[] = {
    0x0020, 0x00FF,
    0x0100, 0x017F,
    0x0400, 0x052F,
    0,
};

void FontManager::LoadFonts(ImGuiIO& io) {
    auto settings = Settings::GetSingleton();
    std::string fontPath = GetFontPath(settings->language);
    ImFontConfig fontConfig;
    fontConfig.PixelSnapH = true;
    bool isCJK = settings->language.find("_zh") != std::string::npos ||
                    settings->language.find("_ja") != std::string::npos ||
                    settings->language.find("_ko") != std::string::npos;
    fontConfig.OversampleH = isCJK ? 2 : 3;
    fontConfig.OversampleV = 2;
    fontConfig.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_NoHinting;
    float baseFontSize = static_cast<float>(settings->fontSize);
    float headingSize = static_cast<float>(settings->headingFontSize);
    const ImWchar* glyphRanges = GetGlyphRanges(settings->language, io);
    baseFont = nullptr;
    headingFont = nullptr;
    io.Fonts->Clear();
    io.Fonts->FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();
    if (!fontPath.empty()) {
        logger::info("Loading base font from: {} at {}px", fontPath, baseFontSize);
        baseFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), baseFontSize, &fontConfig, glyphRanges);
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
        baseFont = io.Fonts->Fonts[0];
    }
    io.Fonts->Build();
}

const ImWchar* FontManager::GetGlyphRanges(const std::string& language, ImGuiIO& io) {
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
    return kEuropeanRanges;
}

std::string FontManager::GetFontPath(const std::string& language) {
    std::string customPath = "Data/SKSE/Plugins/fonts/" + language + ".ttf";
    if (std::filesystem::exists(customPath)) {
        return customPath;
    }
    std::string segoeUIPath = "C:\\Windows\\Fonts\\seguisb.ttf";
    if (std::filesystem::exists(segoeUIPath)) {
        return segoeUIPath;
    }
    return "";
}
