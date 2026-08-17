#pragma once

#include <windows.h>

#include <cstring>
#include <limits>
#include <string>

inline bool isValidUTF8(const char* text)
{
    if (!text) return false;
    const size_t length = std::strlen(text);
    if (length == 0) return true;
    if (length > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0) > 0;
}

inline std::string convertFromANSIToUTF8(const char* text)
{
    if (!text) return {};
    const size_t length = std::strlen(text);
    if (length == 0) return {};
    if (length > static_cast<size_t>(std::numeric_limits<int>::max())) return {};

    const int wideLength = MultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(length), nullptr, 0);
    if (wideLength <= 0) return {};

    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(length), wide.data(), wideLength) != wideLength) {
        return {};
    }

    const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength,
                                               nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return {};

    std::string utf8(static_cast<size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength, utf8.data(), utf8Length,
                            nullptr, nullptr) != utf8Length) {
        return {};
    }
    return utf8;
}
