#pragma once

#include <vector>
#include <string>
#include <algorithm>

namespace PrismaUI::URLWhitelist {

static constexpr std::array<std::string_view, 6> WHITELISTED_DOMAINS = {
    "static.wikia.nocookie.net",    // Fallout wiki images
    "cdn.jsdelivr.net",              // JavaScript CDN (Tailwind, React, etc.)
    "fonts.googleapis.com",           // Google Fonts
    "youtube.com",                   // YouTube videos (youtube.com, www.youtube.com, youtu.be)
    "youtu.be",                      // YouTube short links
    "nexusmods.com",                 // Nexus Mods (any game: fallout4.nexusmods.com, skyrim.nexusmods.com, etc.)
};

inline bool IsWhitelisted(std::string_view domain) {
    return std::any_of(
        WHITELISTED_DOMAINS.begin(),
        WHITELISTED_DOMAINS.end(),
        [domain](std::string_view whitelisted) {
            return domain == whitelisted ||
                   (domain.size() > whitelisted.size() &&
                    domain.substr(domain.size() - whitelisted.size()) == whitelisted &&
                    domain[domain.size() - whitelisted.size() - 1] == '.');
        }
    );
}

inline std::string GenerateImgSrcDirective() {
    std::string directive = "img-src 'self' data: blob: file:";
    for (auto domain : WHITELISTED_DOMAINS) {
        directive += " https://";
        directive += domain;
    }
    directive += ";";
    return directive;
}

inline std::string GenerateScriptSrcDirective() {
    std::string directive = "script-src 'self' 'unsafe-inline' 'unsafe-eval' file: data:";
    for (auto domain : WHITELISTED_DOMAINS) {
        directive += " https://";
        directive += domain;
    }
    directive += ";";
    return directive;
}

inline std::string GenerateStyleSrcDirective() {
    std::string directive = "style-src 'self' 'unsafe-inline' file: data:";
    for (auto domain : WHITELISTED_DOMAINS) {
        directive += " https://";
        directive += domain;
    }
    directive += ";";
    return directive;
}

} // namespace PrismaUI::URLWhitelist
