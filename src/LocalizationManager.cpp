// Handles JSON/CSV localization strings and multi-language translations.

#include "LocalizationManager.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>

static bool IsValidUTF8(const std::string& str) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(str.c_str());
    const size_t len = str.length();
    for (size_t i = 0; i < len;) {
        if (bytes[i] <= 0x7F) {
            i += 1;
        } else if ((bytes[i] & 0xE0) == 0xC0) {
            if (i + 1 >= len || (bytes[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((bytes[i] & 0xF0) == 0xE0) {
            if (i + 2 >= len || (bytes[i + 1] & 0xC0) != 0x80 ||
                (bytes[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((bytes[i] & 0xF8) == 0xF0) {
            if (i + 3 >= len || (bytes[i + 1] & 0xC0) != 0x80 ||
                (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

inline std::string WStrToStr(const std::wstring& wstr) {
#pragma warning(push)
#pragma warning(disable : 4244)
    return std::string(wstr.begin(), wstr.end());
#pragma warning(pop)
}

void LocalizationManager::LoadLanguage(const std::string& languageCode) {
    labels.clear();
    englishFallback.clear();
    std::wstring basePath = L"Data/SKSE/Plugins/Translations/";
    std::wstring langFile = basePath + std::wstring(languageCode.begin(), languageCode.end()) + L".txt";
    std::wstring enFile = basePath + L"CustomMarkers_en.txt";
    if (!std::filesystem::exists(enFile)) {
        enFile = basePath + L"CustomMapMarkers_en.txt";
    }
    logger::info("Loading language file: {}", WStrToStr(langFile));
    if (!LoadLanguageFile(enFile, englishFallback)) {
        logger::warn("Could not load English fallback file: {}", WStrToStr(enFile));
    } else {
        logger::info("English fallback loaded successfully ({} keys)", englishFallback.size());
    }
    if (languageCode != "CustomMarkers_en" && languageCode != "CustomMapMarkers_en") {
        if (LoadLanguageFile(langFile, labels)) {
            logger::info("Language file loaded successfully ({} keys)", labels.size());
        } else {
            logger::warn("Could not load language file, using English");
            labels = englishFallback;
        }
    } else {
        labels = englishFallback;
    }
}

bool LocalizationManager::LoadLanguageFile(const std::wstring& filename,
                                           std::unordered_map<std::string, std::string>& target) {
    if (!std::filesystem::exists(filename)) {
        return false;
    }
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content = content.substr(3);
    }
    if (!IsValidUTF8(content)) {
        logger::error("Translation file is not valid UTF-8, refusing to load: {}",
                      WStrToStr(filename));
        return false;
    }
    std::istringstream stream(content);
    std::string line;
    std::string currentSection;
    while (std::getline(stream, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line[0] == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = line.substr(0, equalPos);
            std::string value = line.substr(equalPos + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            if (!currentSection.empty()) {
                target[currentSection + "." + key] = value;
            }
            target[key] = value;
        }
    }
    return !target.empty();
}

std::string LocalizationManager::GetLabel(const std::string& key) const {
    auto it = labels.find(key);
    if (it != labels.end()) {
        return it->second;
    }
    it = englishFallback.find(key);
    if (it != englishFallback.end()) {
        return it->second;
    }
    logger::warn("Missing translation for key: {}", key);
    return key;
}
