#pragma once

#include "PrismaUI_F4_API.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <cctype>
#include <string>
#include <string_view>

namespace PRISMA_UI_HELPER {

inline void SendPapyrusEvent(RE::TESForm* form, const RE::BSFixedString& eventName,
                             RE::BSScript::IFunctionArguments* args = nullptr)
{
    if (!form) return;

    auto* gameVM = RE::GameVM::GetSingleton();
    if (!gameVM) return;
    auto* vm = gameVM->GetVM().get();
    if (!vm) return;

    auto& policy = vm->GetObjectHandlePolicy();
    const auto handle = policy.GetHandleForObject(form->GetFormType(), form);
    if (handle == policy.EmptyHandle()) return;
    vm->SendCustomEvent(handle, nullptr, eventName, args);
}

inline std::string GetJsonString(std::string_view json, std::string_view key)
{
    std::string search = "\"";
    search += key;
    search += "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string_view::npos) return {};
    pos += search.size();

    std::string output;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) {
            switch (c) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default: return {};
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') return output;
        output.push_back(c);
    }
    return {};
}

inline float GetJsonFloat(std::string_view json, std::string_view key, float fallback = 0.0f)
{
    std::string search = "\"";
    search += key;
    search += "\":";
    size_t pos = json.find(search);
    if (pos == std::string_view::npos) return fallback;
    pos += search.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;

    try {
        size_t consumed = 0;
        const std::string value(json.substr(pos));
        const float parsed = std::stof(value, &consumed);
        return consumed > 0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

inline int GetJsonInt(std::string_view json, std::string_view key, int fallback = 0)
{
    return static_cast<int>(GetJsonFloat(json, key, static_cast<float>(fallback)));
}

inline bool GetJsonBool(std::string_view json, std::string_view key, bool fallback = false)
{
    std::string search = "\"";
    search += key;
    search += "\":";
    size_t pos = json.find(search);
    if (pos == std::string_view::npos) return fallback;
    pos += search.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;

    const auto value = json.substr(pos);
    if (value.starts_with("true")) return true;
    if (value.starts_with("false")) return false;
    return fallback;
}

}
