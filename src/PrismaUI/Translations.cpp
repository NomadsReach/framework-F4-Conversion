#include "Translations.h"

#include <windows.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace PrismaUI::Translations {
namespace {
constexpr std::streamsize kMaxTranslationFile = 16 * 1024 * 1024;

bool IsSafeName(const std::string& value)
{
    if (value.empty() || value.size() > 128 || value.find("..") != std::string::npos) return false;
    for (const unsigned char c : value) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':') return false;
    }
    return true;
}

std::filesystem::path GameDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(path).parent_path();
}

void ParseUtf8Lines(const std::string& text, std::unordered_map<std::string, std::string>& output)
{
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() != '$') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        output[line.substr(0, tab)] = line.substr(tab + 1);
    }
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            size, nullptr, nullptr) != size) return {};
    return result;
}

void ParseUtf16Le(const std::vector<unsigned char>& raw, std::unordered_map<std::string, std::string>& output)
{
    if (raw.size() < 2 || ((raw.size() - 2) % 2) != 0) return;
    std::wstring wide;
    wide.reserve((raw.size() - 2) / 2);
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        const uint16_t code = static_cast<uint16_t>(raw[i]) |
                              static_cast<uint16_t>(static_cast<uint16_t>(raw[i + 1]) << 8u);
        wide.push_back(static_cast<wchar_t>(code));
    }

    std::wistringstream stream(wide);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line.front() != L'$') continue;
        const size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        const std::string key = WideToUtf8(line.substr(0, tab));
        if (!key.empty()) output[key] = WideToUtf8(line.substr(tab + 1));
    }
}

std::string EscapeJsString(const std::string& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() + 8);
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (i + 2 < value.size() && c == 0xE2 && static_cast<unsigned char>(value[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(value[i + 2]) == 0xA8 || static_cast<unsigned char>(value[i + 2]) == 0xA9)) {
            output += static_cast<unsigned char>(value[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
            continue;
        }
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            default:
                if (c < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[(c >> 4) & 0xF]);
                    output.push_back(hex[c & 0xF]);
                } else output.push_back(static_cast<char>(c));
                break;
        }
    }
    return output;
}
}

std::string DetectGameLanguage()
{
    std::vector<std::filesystem::path> candidates;
    wchar_t profile[MAX_PATH]{};
    const DWORD profileLength = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (profileLength > 0 && profileLength < MAX_PATH) {
        const auto myGames = std::filesystem::path(profile) / L"Documents" / L"My Games" / L"Fallout4";
        candidates.push_back(myGames / L"Fallout4Custom.ini");
        candidates.push_back(myGames / L"Fallout4Prefs.ini");
    }
    const auto gameDirectory = GameDirectory();
    if (!gameDirectory.empty()) candidates.push_back(gameDirectory / L"Fallout4.ini");

    for (const auto& path : candidates) {
        char buffer[64]{};
        if (GetPrivateProfileStringA("General", "sLanguage", "", buffer, sizeof(buffer), path.string().c_str()) == 0)
            continue;
        std::string language(buffer);
        for (char& c : language) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (IsSafeName(language)) return language;
    }
    return FALLBACK_LANG;
}

std::unordered_map<std::string, std::string> ParseTranslationFile(const std::string& pluginName,
                                                                   const std::string& lang)
{
    std::unordered_map<std::string, std::string> result;
    if (!IsSafeName(pluginName) || !IsSafeName(lang)) return result;
    const auto gameDirectory = GameDirectory();
    if (gameDirectory.empty()) return result;

    const auto directory = gameDirectory / L"Data" / L"Interface" / L"Translations";
    auto openFile = [&](const std::string& language) {
        return std::ifstream(directory / (pluginName + "_" + language + ".txt"), std::ios::binary);
    };

    std::ifstream file = openFile(lang);
    if (!file.is_open() && lang != FALLBACK_LANG) file = openFile(FALLBACK_LANG);
    if (!file.is_open()) return result;
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    if (size <= 0 || size > kMaxTranslationFile) return result;
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> raw(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(raw.data()), size)) return result;
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) ParseUtf16Le(raw, result);
    else ParseUtf8Lines(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()), result);
    return result;
}

std::string BuildL10NScript(const std::unordered_map<std::string, std::string>& translations)
{
    if (translations.empty()) return {};
    std::string script = "window.L10N={";
    bool first = true;
    for (const auto& [key, value] : translations) {
        if (!first) script.push_back(',');
        first = false;
        script += '"';
        script += EscapeJsString(key);
        script += "\":\"";
        script += EscapeJsString(value);
        script += '"';
    }
    script += "};window.t=function(k){return window.L10N[k]!==undefined?window.L10N[k]:k;};";
    return script;
}

}
