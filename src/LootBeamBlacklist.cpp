#include "PCH.h"
#include "LootBeamBlacklist.h"
#include "PathUtils.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace LootBeamBlacklist {
namespace {
    std::unordered_set<RE::FormID> g_locations;
    std::unordered_set<RE::FormID> g_cells;
    std::unordered_set<RE::FormID> g_references;
    std::unordered_set<RE::FormID> g_containerBases;
    bool g_loaded = false;

    std::filesystem::path GetBlacklistDirectory() {
        const std::filesystem::path runtimePath{ REL::Module::get().filePath().data() };
        return (runtimePath.parent_path() / L"Data" / L"SKSE" / L"Plugins" / L"CMMBlacklist").lexically_normal();
    }

    struct ParseResult {
        std::size_t accepted{ 0 };
        std::size_t skipped{ 0 };
    };

    ParseResult ParseFile(const std::filesystem::path& a_path, const std::string& a_fileUtf8) {
        ParseResult result;
        std::ifstream stream(a_path, std::ios::binary);
        if (!stream.is_open()) {
            return result;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::warn("LootBeamBlacklist: TESDataHandler singleton unavailable while reading '{}'", a_fileUtf8);
            return result;
        }

        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(stream, line)) {
            ++lineNumber;

            // Strip UTF-8 BOM if present on the first line
            if (lineNumber == 1 && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }

            // Ignore full-line or inline ;/# comments
            const auto commentPos = line.find_first_of(";#");
            if (commentPos != std::string::npos) {
                line.erase(commentPos);
            }

            auto trim = [](std::string_view sv) -> std::string_view {
                const auto start = sv.find_first_not_of(" \t\r\n");
                if (start == std::string_view::npos) return {};
                const auto end = sv.find_last_not_of(" \t\r\n");
                return sv.substr(start, end - start + 1);
            };

            auto trimmed = trim(line);
            if (trimmed.empty()) {
                continue;
            }

            // Require exactly one '|'
            const auto firstBar = trimmed.find('|');
            if (firstBar == std::string_view::npos || trimmed.find('|', firstBar + 1) != std::string_view::npos) {
                SKSE::log::warn("LootBeamBlacklist: syntax error in {}:{}: expected exactly one '|' delimiter", a_fileUtf8, lineNumber);
                ++result.skipped;
                continue;
            }

            const auto pluginNameView = trim(trimmed.substr(0, firstBar));
            const auto idPartView = trim(trimmed.substr(firstBar + 1));
            if (pluginNameView.empty() || idPartView.empty()) {
                SKSE::log::warn("LootBeamBlacklist: syntax error in {}:{}: missing plugin name or form ID", a_fileUtf8, lineNumber);
                ++result.skipped;
                continue;
            }

            // Require 0x/0X prefix
            if (idPartView.size() < 3 || !(idPartView[0] == '0' && (idPartView[1] == 'x' || idPartView[1] == 'X'))) {
                SKSE::log::warn("LootBeamBlacklist: syntax error in {}:{}: form ID must begin with '0x' or '0X'", a_fileUtf8, lineNumber);
                ++result.skipped;
                continue;
            }

            const auto hexStr = idPartView.substr(2);
            if (hexStr.empty()) {
                SKSE::log::warn("LootBeamBlacklist: syntax error in {}:{}: form ID has no hexadecimal digits", a_fileUtf8, lineNumber);
                ++result.skipped;
                continue;
            }

            unsigned long long rawId = 0;
            const auto [ptr, ec] = std::from_chars(hexStr.data(), hexStr.data() + hexStr.size(), rawId, 16);
            if (ec != std::errc{} || ptr != hexStr.data() + hexStr.size()) {
                SKSE::log::warn("LootBeamBlacklist: syntax error in {}:{}: invalid or overflowing hexadecimal value '{}'", a_fileUtf8, lineNumber, idPartView);
                ++result.skipped;
                continue;
            }

            if (rawId == 0 || rawId > 0xFFFFFFFFULL) {
                SKSE::log::warn("LootBeamBlacklist: syntax error in {}:{}: form ID out of range or zero (0x{:X})", a_fileUtf8, lineNumber, rawId);
                ++result.skipped;
                continue;
            }

            const auto localId = static_cast<RE::FormID>(rawId);
            const std::string pluginName(pluginNameView);

            auto* form = dataHandler->LookupForm(localId, pluginName);
            if (!form) {
                // Missing plugin or unresolved form: normal for optional mods, increment skipped count without warning
                ++result.skipped;
                continue;
            }

            switch (form->GetFormType()) {
                case RE::FormType::Location:
                    g_locations.insert(form->GetFormID());
                    ++result.accepted;
                    break;
                case RE::FormType::Cell:
                    g_cells.insert(form->GetFormID());
                    ++result.accepted;
                    break;
                case RE::FormType::Reference:
                    g_references.insert(form->GetFormID());
                    ++result.accepted;
                    break;
                case RE::FormType::Container:
                    g_containerBases.insert(form->GetFormID());
                    ++result.accepted;
                    break;
                default:
                    SKSE::log::warn("LootBeamBlacklist: unsupported form type {} for form {:08X} in {}:{}",
                                    static_cast<int>(form->GetFormType()), form->GetFormID(), a_fileUtf8, lineNumber);
                    ++result.skipped;
                    break;
            }
        }

        return result;
    }
}

void Load() {
    if (g_loaded) return;

    g_locations.clear();
    g_cells.clear();
    g_references.clear();
    g_containerBases.clear();

    const auto dir = GetBlacklistDirectory();
    std::error_code ec;
    const bool dirExists = std::filesystem::is_directory(dir, ec);
    if (ec || !dirExists) {
        SKSE::log::info("LootBeamBlacklist: directory '{}' not found, initialized with 0 entries", PathUtils::ToUtf8(dir));
        g_loaded = true;
        return;
    }

    // Parse canonical base file: lootbeam_blacklist.ini
    const auto baseFilePath = dir / L"lootbeam_blacklist.ini";
    const std::string baseFileUtf8 = PathUtils::ToUtf8(baseFilePath.filename());
    if (std::filesystem::is_regular_file(baseFilePath, ec) && !ec) {
        const auto res = ParseFile(baseFilePath, baseFileUtf8);
        SKSE::log::info("LootBeamBlacklist: loaded '{}' (accepted: {}, skipped: {})", baseFileUtf8, res.accepted, res.skipped);
    } else {
        SKSE::log::info("LootBeamBlacklist: base file '{}' not found (accepted: 0, skipped: 0)", baseFileUtf8);
    }

    // Enumerate companion files: regular files ending with _lootbeam_blacklist.ini (case-insensitive)
    struct Companion {
        std::filesystem::path path;
        std::string filenameUtf8;
        std::string sortKey;
    };
    std::vector<Companion> companions;

    std::error_code scanError;
    std::filesystem::directory_iterator it(dir, scanError);
    const std::filesystem::directory_iterator end;
    for (; !scanError && it != end; it.increment(scanError)) {
        const auto& entry = *it;
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) continue;

        const auto filename = entry.path().filename();
        const auto filenameUtf8 = PathUtils::ToUtf8(filename);

        std::string lowerName = filenameUtf8;
        for (char& c : lowerName) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        constexpr std::string_view kSuffix = "_lootbeam_blacklist.ini";
        constexpr std::string_view kPrimary = "lootbeam_blacklist.ini";

        if (lowerName == kPrimary) {
            continue;
        }

        if (lowerName.size() >= kSuffix.size() &&
            lowerName.compare(lowerName.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
            companions.push_back({ entry.path(), filenameUtf8, lowerName });
        }
    }
    if (scanError) {
        SKSE::log::warn("LootBeamBlacklist: stopped scanning '{}': {}",
                        PathUtils::ToUtf8(dir), scanError.message());
    }

    std::sort(companions.begin(), companions.end(), [](const Companion& a, const Companion& b) {
        return a.sortKey < b.sortKey;
    });

    for (const auto& comp : companions) {
        const auto res = ParseFile(comp.path, comp.filenameUtf8);
        SKSE::log::info("LootBeamBlacklist: loaded '{}' (accepted: {}, skipped: {})", comp.filenameUtf8, res.accepted, res.skipped);
    }

    SKSE::log::info("LootBeamBlacklist: complete. Totals - locations: {}, cells: {}, references: {}, container bases: {}",
                    g_locations.size(), g_cells.size(), g_references.size(), g_containerBases.size());

    g_loaded = true;
}

bool IsEnvironmentBlacklisted(const RE::TESObjectCELL* a_cell) {
    if (!g_loaded || !a_cell) return false;

    static RE::FormID s_cachedCellID = 0;
    static bool s_cachedResult = false;
    const auto cellID = a_cell->GetFormID();
    if (cellID == s_cachedCellID) return s_cachedResult;

    s_cachedCellID = cellID;
    s_cachedResult = false;
    if (g_cells.contains(cellID)) {
        s_cachedResult = true;
        return true;
    }

    std::unordered_set<RE::FormID> visited;
    auto* location = a_cell->GetLocation();
    for (std::size_t depth = 0; location && depth < 64; ++depth) {
        const auto id = location->GetFormID();
        if (!visited.insert(id).second) {
            static bool s_warnedCycle = false;
            if (!s_warnedCycle) {
                s_warnedCycle = true;
                SKSE::log::warn("LootBeamBlacklist: cycle detected in location hierarchy at formID {:08X}", id);
            }
            return false;
        }
        if (g_locations.contains(id)) {
            s_cachedResult = true;
            return true;
        }
        location = location->parentLoc;
        if (depth == 63 && location) {
            static bool s_warnedDepth = false;
            if (!s_warnedDepth) {
                s_warnedDepth = true;
                SKSE::log::warn("LootBeamBlacklist: depth limit (64) reached traversing location hierarchy for cell {:08X}", a_cell->GetFormID());
            }
        }
    }
    return s_cachedResult;
}

bool IsReferenceBlacklisted(const RE::TESObjectREFR& a_ref, bool* a_baseMatch) {
    if (a_baseMatch) *a_baseMatch = false;
    if (!g_loaded) return false;
    if (g_references.contains(a_ref.GetFormID())) return true;
    const auto* base = a_ref.GetBaseObject();
    if (base && g_containerBases.contains(base->GetFormID())) {
        if (a_baseMatch) *a_baseMatch = true;
        return true;
    }
    return false;
}
}
