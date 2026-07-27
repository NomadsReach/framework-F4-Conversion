#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace PrismaUI::VirtualFilePolicy
{
    inline constexpr std::size_t kMaximumPathBytes = 8192;

    [[nodiscard]] inline bool EqualsInsensitive(
        std::string_view left,
        std::string_view right) noexcept
    {
        return left.size() == right.size() &&
               std::equal(
                   left.begin(),
                   left.end(),
                   right.begin(),
                   [](unsigned char first, unsigned char second) {
                       return std::tolower(first) ==
                              std::tolower(second);
                   });
    }

    [[nodiscard]] inline int HexValue(char value) noexcept
    {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    [[nodiscard]] inline bool IsReservedDeviceName(
        std::string_view segment) noexcept
    {
        const auto extension = segment.find('.');
        const auto stem = segment.substr(0, extension);
        std::array<char, 8> upper{};
        if (stem.empty() || stem.size() >= upper.size()) {
            return false;
        }
        for (std::size_t index = 0; index < stem.size(); ++index) {
            upper[index] = static_cast<char>(std::toupper(
                static_cast<unsigned char>(stem[index])));
        }
        const std::string_view name(upper.data(), stem.size());
        if (name == "CON" ||
            name == "PRN" ||
            name == "AUX" ||
            name == "NUL" ||
            name == "CLOCK$") {
            return true;
        }
        return name.size() == 4 &&
               (name.starts_with("COM") ||
                name.starts_with("LPT")) &&
               name[3] >= '1' &&
               name[3] <= '9';
    }

    [[nodiscard]] inline bool ValidateEncodedPath(
        std::string_view encoded,
        bool leadingSlash) noexcept
    {
        try {
            if (encoded.empty() ||
                encoded.size() > kMaximumPathBytes ||
                (leadingSlash ?
                     encoded.front() != '/' :
                     encoded.front() == '/' ||
                         encoded.front() == '\\')) {
                return false;
            }

            std::string decoded;
            decoded.reserve(encoded.size());
            for (std::size_t index = 0;
                 index < encoded.size();
                 ++index) {
                auto character =
                    static_cast<unsigned char>(encoded[index]);
                if (character == '%') {
                    if (index + 2 >= encoded.size()) {
                        return false;
                    }
                    const auto high = HexValue(encoded[index + 1]);
                    const auto low = HexValue(encoded[index + 2]);
                    if (high < 0 || low < 0) {
                        return false;
                    }
                    character = static_cast<unsigned char>(
                        (high << 4) | low);
                    index += 2;
                }
                if (character < 0x20u ||
                    character == 0x7Fu ||
                    character == '\\' ||
                    character == ':' ||
                    character == '%' ||
                    character == '?' ||
                    character == '#') {
                    return false;
                }
                decoded.push_back(static_cast<char>(character));
            }

            std::size_t start = leadingSlash ? 1 : 0;
            if (start == decoded.size()) {
                return true;
            }
            while (start <= decoded.size()) {
                const auto separator = decoded.find('/', start);
                const auto segment = std::string_view(decoded).substr(
                    start,
                    separator == std::string::npos ?
                        std::string::npos :
                        separator - start);
                if (segment.empty() ||
                    segment == "." ||
                    segment == ".." ||
                    segment.back() == ' ' ||
                    segment.back() == '.' ||
                    IsReservedDeviceName(segment)) {
                    return false;
                }
                if (separator == std::string::npos) {
                    break;
                }
                start = separator + 1;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] inline bool IsSafeRelativePath(
        std::string_view value) noexcept
    {
        const auto suffix = value.find_first_of("?#");
        return ValidateEncodedPath(
            value.substr(0, suffix),
            false);
    }

    [[nodiscard]] inline bool IsSafeFileUrl(
        std::string_view url,
        std::string_view parsedHost) noexcept
    {
        try {
            if (url.size() > kMaximumPathBytes ||
                url.size() < 8 ||
                !EqualsInsensitive(url.substr(0, 7), "file://") ||
                (!parsedHost.empty() &&
                 !EqualsInsensitive(parsedHost, "localhost"))) {
                return false;
            }

            constexpr std::size_t authorityStart = 7;
            const auto pathStart =
                url.find_first_of("/?#", authorityStart);
            if (pathStart == std::string_view::npos ||
                url[pathStart] != '/') {
                return false;
            }
            const auto authority = url.substr(
                authorityStart,
                pathStart - authorityStart);
            if ((parsedHost.empty() && !authority.empty()) ||
                (!parsedHost.empty() &&
                 !EqualsInsensitive(authority, parsedHost))) {
                return false;
            }

            const auto suffix = url.find_first_of("?#", pathStart);
            return ValidateEncodedPath(
                url.substr(
                    pathStart,
                    suffix == std::string_view::npos ?
                        std::string_view::npos :
                        suffix - pathStart),
                true);
        } catch (...) {
            return false;
        }
    }
}
