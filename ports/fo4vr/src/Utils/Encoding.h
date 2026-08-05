#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace PrismaUI::Encoding
{
    [[nodiscard]] inline bool IsValidUtf8(const char* value) noexcept
    {
        if (!value) {
            return true;
        }
        return MultiByteToWideChar(
                   CP_UTF8,
                   MB_ERR_INVALID_CHARS,
                   value,
                   -1,
                   nullptr,
                   0) != 0;
    }

    [[nodiscard]] inline std::string AnsiToUtf8(const char* value)
    {
        if (!value) {
            return {};
        }

        const auto wideSize =
            MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
        if (wideSize <= 0) {
            return {};
        }

        std::vector<wchar_t> wide(static_cast<std::size_t>(wideSize));
        if (MultiByteToWideChar(
                CP_ACP,
                0,
                value,
                -1,
                wide.data(),
                wideSize) == 0) {
            return {};
        }

        const auto utf8Size = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8Size <= 0) {
            return {};
        }

        std::vector<char> utf8(static_cast<std::size_t>(utf8Size));
        if (WideCharToMultiByte(
                CP_UTF8,
                0,
                wide.data(),
                -1,
                utf8.data(),
                utf8Size,
                nullptr,
                nullptr) == 0) {
            return {};
        }
        return std::string(utf8.data());
    }
}
