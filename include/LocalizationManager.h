#pragma once

// Handles JSON/CSV localization strings and multi-language translations.


#include <format>
#include <string>
#include <unordered_map>

class LocalizationManager {
public:
    static LocalizationManager* GetSingleton() {
        static LocalizationManager singleton;
        return &singleton;
    }
    void LoadLanguage(const std::string& languageCode);
    std::string GetLabel(const std::string& key) const;
    static std::string Get(const std::string& key) {
        return GetSingleton()->GetLabel(key);
    }
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
    bool LoadLanguageFile(const std::wstring& filename, std::unordered_map<std::string, std::string>& target);
    std::unordered_map<std::string, std::string> labels;
    std::unordered_map<std::string, std::string> englishFallback;
};
