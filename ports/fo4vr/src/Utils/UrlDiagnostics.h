#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace PrismaUI::UrlDiagnostics
{
    [[nodiscard]] inline bool StartsWithInsensitive(
        std::string_view value,
        std::string_view prefix) noexcept
    {
        if (value.size() < prefix.size()) {
            return false;
        }
        for (std::size_t index = 0; index < prefix.size(); ++index) {
            const auto left = static_cast<unsigned char>(value[index]);
            const auto right = static_cast<unsigned char>(prefix[index]);
            if (std::tolower(left) != std::tolower(right)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline std::string Sanitize(std::string_view input) noexcept
    {
        try {
            constexpr std::size_t maximumLength = 384;

            if (StartsWithInsensitive(input, "data:")) {
                return "data:<redacted>";
            }
            if (StartsWithInsensitive(input, "blob:")) {
                return "blob:<redacted>";
            }
            if (StartsWithInsensitive(input, "javascript:")) {
                return "javascript:<redacted>";
            }

            input = input.substr(0, input.find_first_of("?#"));
            std::string output;
            const auto separator = input.find("://");
            if (separator == std::string_view::npos) {
                output.assign(input);
            } else {
                const auto authorityStart = separator + 3;
                const auto slash = input.find('/', authorityStart);
                const auto authorityEnd =
                    slash == std::string_view::npos ? input.size() : slash;
                const auto at = input.rfind('@', authorityEnd);
                output.append(input.substr(0, authorityStart));
                output.append(input.substr(
                    at != std::string_view::npos && at >= authorityStart ?
                        at + 1 :
                        authorityStart));
            }

            std::replace_if(
                output.begin(),
                output.end(),
                [](char character) {
                    return std::iscntrl(
                               static_cast<unsigned char>(character)) != 0;
                },
                '_');

            if (output.size() > maximumLength) {
                output.resize(maximumLength - 3);
                output.append("...");
            }
            return output.empty() ? "<empty-url>" : output;
        } catch (...) {
            return "<url-unavailable>";
        }
    }
}
