#pragma once

#include <format>
#include <string>
#include <unordered_map>

class LocalizationManager {
public:
    static LocalizationManager* GetSingleton() {
        static LocalizationManager singleton;
        return &singleton;
    }

    // Load language file based on settings
    void LoadLanguage(const std::string& languageCode);

    // Get localized string by key
    std::string GetLabel(const std::string& key) const;

    // Shorthand so draw sites read as one call. Ported from Party Sheet.
    static std::string Get(const std::string& key) {
        return GetSingleton()->GetLabel(key);
    }

    // Localized format string: the VALUE carries the placeholders, so a
    // translator can move them ("Custom Marker {}" vs "{} Marcador"). Uses
    // vformat because the format string is only known at runtime, and swallows
    // format_error so a bad translation prints the raw pattern instead of
    // throwing out of a Present-hook draw call.
    template <class... Args>
    static std::string Format(const std::string& key, Args&&... args) {
        const std::string pattern = Get(key);
        try {
            return std::vformat(pattern, std::make_format_args(args...));
        } catch (const std::format_error&) {
            return pattern;
        }
    }

private:
    LocalizationManager() = default;
    LocalizationManager(const LocalizationManager&) = delete;
    LocalizationManager& operator=(const LocalizationManager&) = delete;

    // Load specific language file
    bool LoadLanguageFile(const std::wstring& filename, std::unordered_map<std::string, std::string>& target);

    std::unordered_map<std::string, std::string> labels;          // Current language
    std::unordered_map<std::string, std::string> englishFallback; // English fallback
};