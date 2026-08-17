#pragma once

#include <string>
#include <vector>
#include <windows.h>

inline bool isValidUTF8(const char* str) {
    if (!str) {
        return true;
    }
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1, nullptr, 0);
    return len != 0;
}

inline std::string convertFromANSIToUTF8(const char* str) {
    if (!str) {
        return "";
    }

    int wideCharLen = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0);
    if (wideCharLen == 0) {
        return "";
    }

    std::vector<wchar_t> wideCharBuffer(wideCharLen);
    MultiByteToWideChar(CP_ACP, 0, str, -1, wideCharBuffer.data(), wideCharLen);

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideCharBuffer.data(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len == 0) {
        return "";
    }

    std::vector<char> utf8Buffer(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, wideCharBuffer.data(), -1, utf8Buffer.data(), utf8Len, nullptr, nullptr);
    return std::string(utf8Buffer.data());
}
