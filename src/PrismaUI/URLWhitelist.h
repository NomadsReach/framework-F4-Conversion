#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace PrismaUI::URLWhitelist {

inline constexpr std::array<std::string_view, 6> WHITELISTED_DOMAINS = {
    "static.wikia.nocookie.net",
    "cdn.jsdelivr.net",
    "fonts.googleapis.com",
    "youtube.com",
    "youtu.be",
    "nexusmods.com",
};

inline bool IsWhitelisted(std::string_view domain) {
    return std::any_of(WHITELISTED_DOMAINS.begin(), WHITELISTED_DOMAINS.end(), [domain](std::string_view allowed) {
        return domain == allowed ||
               (domain.size() > allowed.size() && domain.ends_with(allowed) &&
                domain[domain.size() - allowed.size() - 1] == '.');
    });
}

inline std::string GenerateSourceDirective(std::string_view prefix) {
    std::string directive(prefix);
    for (const auto domain : WHITELISTED_DOMAINS) {
        directive += " https://";
        directive += domain;
    }
    directive += ';';
    return directive;
}

inline std::string GenerateImgSrcDirective() {
    return GenerateSourceDirective("img-src 'self' data: blob: file:");
}

inline std::string GenerateScriptSrcDirective() {
    return GenerateSourceDirective("script-src 'self' 'unsafe-inline' 'unsafe-eval' file: data:");
}

inline std::string GenerateStyleSrcDirective() {
    return GenerateSourceDirective("style-src 'self' 'unsafe-inline' file: data:");
}

inline std::string GenerateFontSrcDirective() {
    return GenerateSourceDirective("font-src 'self' data: file:");
}

}
