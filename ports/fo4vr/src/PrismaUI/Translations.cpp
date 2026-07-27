#include "PCH.h"

#include "PrismaUI/Translations.h"

#include "PrismaUI/Core.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace PrismaUI::Translations
{
    namespace
    {
        constexpr std::string_view fallbackLanguage = "en";
        constexpr std::uintmax_t maximumFileBytes =
            4u * 1024u * 1024u;

        [[nodiscard]] bool IsSafePluginName(
            std::string_view pluginName) noexcept
        {
            constexpr std::string_view forbidden = "<>:\"/\\|?*";
            return !pluginName.empty() &&
                   pluginName.size() <= 128 &&
                   pluginName.front() != '.' &&
                   pluginName.back() != ' ' &&
                   pluginName.back() != '.' &&
                   pluginName.find("..") == std::string_view::npos &&
                   std::all_of(
                       pluginName.begin(),
                       pluginName.end(),
                       [forbidden](unsigned char character) {
                           return character >= 0x20 &&
                                  forbidden.find(
                                      static_cast<char>(character)) ==
                                      std::string_view::npos;
                       });
        }

        [[nodiscard]] std::optional<std::filesystem::path>
            GameDirectory() noexcept
        {
            try {
                std::wstring executablePath(32768, L'\0');
                const auto length = GetModuleFileNameW(
                    nullptr,
                    executablePath.data(),
                    static_cast<DWORD>(executablePath.size()));
                if (length == 0 || length >= executablePath.size()) {
                    return std::nullopt;
                }
                executablePath.resize(length);
                return std::filesystem::path(executablePath).parent_path();
            } catch (...) {
                return std::nullopt;
            }
        }

        [[nodiscard]] std::string DetectLanguage()
        {
            try {
                const auto* setting =
                    RE::GetINISetting("sLanguage:General");
                if (setting &&
                    setting->GetType() ==
                        RE::Setting::SETTING_TYPE::kString) {
                    auto language = std::string(setting->GetString());
                    std::transform(
                        language.begin(),
                        language.end(),
                        language.begin(),
                        [](unsigned char character) {
                            return static_cast<char>(
                                std::tolower(character));
                        });
                    if (!language.empty() &&
                        language.size() <= 16 &&
                        std::all_of(
                            language.begin(),
                            language.end(),
                            [](unsigned char character) {
                                return std::isalnum(character) != 0 ||
                                       character == '-';
                            })) {
                        return language;
                    }
                }
            } catch (...) {
            }
            return std::string(fallbackLanguage);
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> ReadFile(
            const std::filesystem::path& path)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size < 2 || size > maximumFileBytes) {
                return std::nullopt;
            }

            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                return std::nullopt;
            }

            std::vector<std::byte> bytes(
                static_cast<std::size_t>(size));
            if (!stream.read(
                    reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
                return std::nullopt;
            }
            return bytes;
        }

        [[nodiscard]] std::string WideToUtf8(
            std::wstring_view value)
        {
            if (value.empty()) {
                return {};
            }
            const auto required = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (required <= 0) {
                return {};
            }
            std::string result(static_cast<std::size_t>(required), '\0');
            if (WideCharToMultiByte(
                    CP_UTF8,
                    WC_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required,
                    nullptr,
                    nullptr) != required) {
                return {};
            }
            return result;
        }

        void ParseUtf8(
            std::string_view content,
            std::map<std::string, std::string>& entries)
        {
            std::istringstream stream{std::string(content)};
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty() || line.front() != '$') {
                    continue;
                }
                const auto tab = line.find('\t');
                if (tab == std::string::npos || tab == 0) {
                    continue;
                }
                entries[line.substr(0, tab)] = line.substr(tab + 1);
            }
        }

        void ParseUtf16Le(
            const std::vector<std::byte>& bytes,
            std::map<std::string, std::string>& entries)
        {
            static_assert(sizeof(wchar_t) == 2);
            if (bytes.size() < 4 || (bytes.size() - 2) % 2 != 0) {
                return;
            }

            std::wstring content;
            content.reserve((bytes.size() - 2) / 2);
            for (std::size_t offset = 2;
                 offset + 1 < bytes.size();
                 offset += 2) {
                const auto low =
                    std::to_integer<std::uint8_t>(bytes[offset]);
                const auto high =
                    std::to_integer<std::uint8_t>(bytes[offset + 1]);
                content.push_back(static_cast<wchar_t>(
                    low | (static_cast<std::uint16_t>(high) << 8u)));
            }

            std::wistringstream stream(content);
            std::wstring line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }
                if (line.empty() || line.front() != L'$') {
                    continue;
                }
                const auto tab = line.find(L'\t');
                if (tab == std::wstring::npos || tab == 0) {
                    continue;
                }

                auto key = WideToUtf8(
                    std::wstring_view(line).substr(0, tab));
                auto value = WideToUtf8(
                    std::wstring_view(line).substr(tab + 1));
                if (!key.empty()) {
                    entries[std::move(key)] = std::move(value);
                }
            }
        }

        [[nodiscard]] std::string EscapeJavaScript(
            std::string_view value)
        {
            std::ostringstream escaped;
            escaped << std::hex << std::uppercase;

            for (std::size_t index = 0; index < value.size(); ++index) {
                const auto character =
                    static_cast<unsigned char>(value[index]);
                switch (character) {
                case '\\':
                    escaped << "\\\\";
                    break;
                case '"':
                    escaped << "\\\"";
                    break;
                case '\b':
                    escaped << "\\b";
                    break;
                case '\f':
                    escaped << "\\f";
                    break;
                case '\n':
                    escaped << "\\n";
                    break;
                case '\r':
                    escaped << "\\r";
                    break;
                case '\t':
                    escaped << "\\t";
                    break;
                default:
                    if (character < 0x20) {
                        escaped
                            << "\\u"
                            << std::setw(4)
                            << std::setfill('0')
                            << static_cast<unsigned>(character);
                    } else if (
                        index + 2 < value.size() &&
                        character == 0xE2 &&
                        static_cast<unsigned char>(value[index + 1]) ==
                            0x80 &&
                        (static_cast<unsigned char>(value[index + 2]) ==
                             0xA8 ||
                         static_cast<unsigned char>(value[index + 2]) ==
                             0xA9)) {
                        escaped <<
                            (static_cast<unsigned char>(value[index + 2]) ==
                                     0xA8 ?
                                 "\\u2028" :
                                 "\\u2029");
                        index += 2;
                    } else {
                        escaped << static_cast<char>(character);
                    }
                    break;
                }
            }
            return escaped.str();
        }

        [[nodiscard]] std::shared_ptr<const std::string> BuildScript(
            const std::map<std::string, std::string>& entries)
        {
            if (entries.empty()) {
                return nullptr;
            }

            std::string script = "window.L10N={";
            bool first = true;
            for (const auto& [key, value] : entries) {
                if (!first) {
                    script.push_back(',');
                }
                first = false;
                script += '"';
                script += EscapeJavaScript(key);
                script += "\":\"";
                script += EscapeJavaScript(value);
                script += '"';
            }
            script +=
                "};window.t=function(k){return "
                "Object.prototype.hasOwnProperty.call(window.L10N,k)?"
                "window.L10N[k]:k;};";
            return std::make_shared<const std::string>(
                std::move(script));
        }
    }

    std::shared_ptr<const std::string> LoadTranslationScript(
        const std::string& pluginName)
    {
        if (!IsSafePluginName(pluginName)) {
            logger::warn(
                "Rejected unsafe PrismaUI translation plugin name");
            return nullptr;
        }

        const auto gameDirectory = GameDirectory();
        if (!gameDirectory) {
            logger::warn(
                "Could not resolve the Fallout 4 VR game directory for translations");
            return nullptr;
        }

        const auto language = DetectLanguage();
        const auto root =
            *gameDirectory / "Data" / "Interface" / "Translations";
        auto path =
            root / (pluginName + "_" + language + ".txt");
        auto bytes = ReadFile(path);
        if (!bytes && language != fallbackLanguage) {
            path = root /
                (pluginName + "_" +
                 std::string(fallbackLanguage) + ".txt");
            bytes = ReadFile(path);
        }
        if (!bytes) {
            logger::warn(
                "No translation file found for '{}' (language '{}')",
                pluginName,
                language);
            return nullptr;
        }

        std::map<std::string, std::string> entries;
        if (bytes->size() >= 2 &&
            std::to_integer<std::uint8_t>((*bytes)[0]) == 0xFF &&
            std::to_integer<std::uint8_t>((*bytes)[1]) == 0xFE) {
            ParseUtf16Le(*bytes, entries);
        } else {
            std::string_view content(
                reinterpret_cast<const char*>(bytes->data()),
                bytes->size());
            if (content.size() >= 3 &&
                static_cast<unsigned char>(content[0]) == 0xEF &&
                static_cast<unsigned char>(content[1]) == 0xBB &&
                static_cast<unsigned char>(content[2]) == 0xBF) {
                content.remove_prefix(3);
            }
            ParseUtf8(content, entries);
        }

        auto script = BuildScript(entries);
        if (script) {
            logger::info(
                "Loaded {} translations for '{}'",
                entries.size(),
                pluginName);
        }
        return script;
    }

    bool InjectForCurrentWindow(
        Core::PrismaView& view,
        ultralight::View* ultralightView) noexcept
    {
        if (!ultralightView ||
            !view.windowObjectReady.load(std::memory_order_acquire)) {
            return false;
        }

        try {
            std::shared_ptr<const std::string> script;
            std::uint64_t revision = 0;
            {
                std::lock_guard lock(view.translationMutex);
                revision = view.translationRevision.load(
                    std::memory_order_acquire);
                script = view.translationScript;
            }
            if (revision == 0 || !script || script->empty() ||
                view.injectedTranslationRevision.load(
                    std::memory_order_acquire) >= revision) {
                return false;
            }

            ultralight::String exception;
            ultralightView->EvaluateScript(
                ultralight::String(script->c_str()),
                &exception);
            if (!exception.empty()) {
                logger::error(
                    "View [{}] translation injection failed",
                    view.id);
                return false;
            }

            view.injectedTranslationRevision.store(
                revision,
                std::memory_order_release);
            return true;
        } catch (...) {
            logger::error(
                "View [{}] translation injection raised an exception",
                view.id);
            return false;
        }
    }
}
