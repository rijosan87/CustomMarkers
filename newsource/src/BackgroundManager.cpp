// src/BackgroundManager.cpp
// See include/BackgroundManager.h for the naming contract.
//
// STATUS
//   Working: startup scan of Data/Interface/CustomMapMarkers/Backgrounds/ for
//            ListPanel-<n>.<png|jpg|jpeg|dds|bmp>, sorted by the numeric suffix,
//            handed out 1-based. Consumed by MapMarkerRenderer::DrawListPanel
//            when Settings::listPanelUseCustomBg is on.
//   Notes:   the scan is startup-only. A file dropped in while the game runs
//            is not picked up until the next launch, which matches Party Sheet
//            and keeps the picker off the filesystem every frame.

#include "BackgroundManager.h"
#include "TextureManager.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace logger = SKSE::log;
namespace fs = std::filesystem;

namespace BackgroundManager {

    static constexpr const char* BACKGROUNDS_PATH = "Data\\Interface\\CustomMarkers\\Backgrounds\\";

    static const char* GetBackgroundsPath() {
        static const std::string kPrimary = "Data\\Interface\\CustomMarkers\\Backgrounds\\";
        static const std::string kLegacy = "Data\\Interface\\CustomMapMarkers\\Backgrounds\\";
        std::error_code ec;
        if (fs::exists(kPrimary, ec)) {
            return kPrimary.c_str();
        }
        if (fs::exists(kLegacy, ec)) {
            return kLegacy.c_str();
        }
        return kPrimary.c_str();
    }

    static constexpr const char* PREFIX_LISTPANEL = "ListPanel-";

    static std::vector<std::string> g_listPanelBgs;
    static bool        g_initialized = false;
    static std::string g_emptyString;

    // Trailing number after the prefix ("ListPanel-2.png" -> 2). 0 on anything
    // unparseable, which sorts those first rather than throwing out of the scan.
    static int ExtractIndex(const std::string& filename, const std::string& prefix) {
        try {
            const std::string stem = fs::path(filename).stem().string();
            if (stem.length() <= prefix.length()) return 0;
            return std::stoi(stem.substr(prefix.length()));
        } catch (...) {
            return 0;
        }
    }

    // Case-insensitive: users rename files by hand and "listpanel-1.png" should
    // still work.
    static bool StartsWithNoCase(const std::string& str, const std::string& prefix) {
        if (str.length() < prefix.length()) return false;
        for (std::size_t i = 0; i < prefix.length(); ++i) {
            if (std::tolower(static_cast<unsigned char>(str[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) {
                return false;
            }
        }
        return true;
    }

    void Init() {
        if (g_initialized) return;
        g_listPanelBgs.clear();

        std::error_code ec;  // non-throwing overloads: a missing/unreadable
                             // folder is normal (the user may ship no art).
        const char* bgPath = GetBackgroundsPath();
        if (fs::exists(bgPath, ec)) {
            for (const auto& entry : fs::directory_iterator(bgPath, ec)) {
                if (!entry.is_regular_file(ec)) continue;

                const std::string filename = entry.path().filename().string();
                std::string ext = entry.path().extension().string();
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" &&
                    ext != ".dds" && ext != ".bmp") {
                    continue;
                }

                if (StartsWithNoCase(filename, PREFIX_LISTPANEL)) {
                    g_listPanelBgs.push_back(filename);
                }
            }
        } else {
            logger::info("BackgroundManager: no Backgrounds folder at {}", bgPath);
        }

        std::sort(g_listPanelBgs.begin(), g_listPanelBgs.end(),
                  [](const std::string& a, const std::string& b) {
                      return ExtractIndex(a, PREFIX_LISTPANEL) < ExtractIndex(b, PREFIX_LISTPANEL);
                  });

        logger::info("BackgroundManager: {} list panel background(s)", g_listPanelBgs.size());
        g_initialized = true;
    }

    const std::vector<std::string>& GetBackgrounds(Type) {
        // One type today. The switch Party Sheet has would be noise here; add
        // it back when a second surface actually wants its own folder prefix.
        return g_listPanelBgs;
    }

    int GetBackgroundCount(Type type) {
        return static_cast<int>(GetBackgrounds(type).size());
    }

    const std::string& GetBackgroundName(Type type, int index) {
        const auto& list = GetBackgrounds(type);
        if (index >= 1 && index <= static_cast<int>(list.size())) {
            return list[index - 1];  // 1-based: 0 means "none picked yet"
        }
        return g_emptyString;
    }

    ID3D11ShaderResourceView* GetBackground(Type type, int index) {
        const std::string& filename = GetBackgroundName(type, index);
        if (filename.empty()) return nullptr;
        return TextureManager::GetTexture(std::string(GetBackgroundsPath()) + filename);
    }

    std::vector<BackgroundEntry> GetBackgroundEntries(Type type) {
        std::vector<BackgroundEntry> entries;
        for (const auto& filename : GetBackgrounds(type)) {
            // Drop failures rather than pushing a null texture: an ImageButton
            // with a null SRV draws an invisible-but-clickable cell.
            if (auto* tex = TextureManager::GetTexture(std::string(GetBackgroundsPath()) + filename)) {
                entries.push_back({ filename, tex });
            }
        }
        return entries;
    }

}  // namespace BackgroundManager
