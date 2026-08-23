#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace PrismaUI::URLWhitelist {

inline constexpr std::array<std::string_view, 1> SCRIPT_DOMAINS = {
    "cdn.jsdelivr.net",
};

inline constexpr std::array<std::string_view, 2> STYLE_DOMAINS = {
    "cdn.jsdelivr.net",
    "fonts.googleapis.com",
};

inline constexpr std::array<std::string_view, 6> IMAGE_DOMAINS = {
    "static.wikia.nocookie.net",
    "cdn.jsdelivr.net",
    "youtube.com",
    "youtu.be",
    "nexusmods.com",
    "staticdelivery.nexusmods.com",
};

inline constexpr std::array<std::string_view, 2> FONT_DOMAINS = {
    "cdn.jsdelivr.net",
    "fonts.gstatic.com",
};

template <std::size_t N>
inline bool IsWhitelisted(std::string_view domain, const std::array<std::string_view, N>& domains)
{
    return std::any_of(domains.begin(), domains.end(), [domain](std::string_view allowed) {
        return domain == allowed ||
               (domain.size() > allowed.size() && domain.ends_with(allowed) &&
                domain[domain.size() - allowed.size() - 1] == '.');
    });
}

template <std::size_t N>
inline void AppendHttpsDomains(std::string& directive, const std::array<std::string_view, N>& domains)
{
    for (const auto domain : domains) {
        directive += " https://";
        directive += domain;
    }
}

inline std::string GenerateCsp()
{
    std::string csp = "default-src 'self' 'unsafe-inline' 'unsafe-eval' file: data: blob:; connect-src 'none'; ";

    csp += "script-src 'self' 'unsafe-inline' 'unsafe-eval' file: data:";
    AppendHttpsDomains(csp, SCRIPT_DOMAINS);
    csp += "; ";

    csp += "img-src 'self' data: blob: file:";
    AppendHttpsDomains(csp, IMAGE_DOMAINS);
    csp += "; ";

    csp += "style-src 'self' 'unsafe-inline' file: data:";
    AppendHttpsDomains(csp, STYLE_DOMAINS);
    csp += "; ";

    csp += "font-src 'self' data: file:";
    AppendHttpsDomains(csp, FONT_DOMAINS);
    csp += "; media-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none'; form-action 'none'; base-uri 'none';";
    return csp;
}

}
