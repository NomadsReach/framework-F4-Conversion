#pragma once

#include <string>
#include <unordered_map>

namespace PrismaUI::Translations {
    constexpr const char* FALLBACK_LANG = "en";

    std::string DetectGameLanguage();
    std::unordered_map<std::string, std::string> ParseTranslationFile(const std::string& pluginName,
                                                                      const std::string& lang);
    std::string BuildL10NScript(const std::unordered_map<std::string, std::string>& translations);
}
