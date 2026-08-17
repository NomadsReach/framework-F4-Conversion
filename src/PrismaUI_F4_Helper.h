#pragma once

#include "PrismaUI_F4_API.h"

#include <RE/Fallout.h>
#include <string>
#include <string_view>

namespace PRISMA_UI_HELPER {

    inline void SendPapyrusEvent(RE::TESForm* form, const RE::BSFixedString& eventName,
                                 RE::BSScript::IFunctionArguments* args = nullptr) {
        if (!form) {
            return;
        }

        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) {
            return;
        }

        auto* vm = gameVM->GetVM().get();
        if (!vm) {
            return;
        }

        auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(form->GetFormType(), form);
        if (handle == vm->GetObjectHandlePolicy().EmptyHandle()) {
            return;
        }

        vm->SendCustomEvent(handle, nullptr, eventName, args);
    }

    // The raw BindUIEvent callback cannot capture targetForm or papyrusEventName. Kept for source compatibility.
    inline void BindUIEventToPapyrus(PRISMA_UI_API::IVPrismaUI4* api, PrismaView view, const char* jsFunctionName,
                                     [[maybe_unused]] RE::TESForm* targetForm,
                                     [[maybe_unused]] const RE::BSFixedString& papyrusEventName) {
        if (!api || !targetForm) {
            return;
        }

        api->BindUIEvent(view, jsFunctionName, [](const char* /*json*/) {});
    }

    inline std::string GetJsonString(std::string_view json, std::string_view key) {
        std::string search = "\"";
        search += key;
        search += "\":\"";
        auto pos = json.find(search);
        if (pos == std::string_view::npos) {
            return {};
        }
        pos += search.size();
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) {
            return {};
        }
        return std::string(json.substr(pos, end - pos));
    }

    inline float GetJsonFloat(std::string_view json, std::string_view key, float fallback = 0.0f) {
        std::string search = "\"";
        search += key;
        search += "\":";
        auto pos = json.find(search);
        if (pos == std::string_view::npos) {
            return fallback;
        }
        pos += search.size();
        try {
            return std::stof(std::string(json.substr(pos)));
        } catch (...) {
            return fallback;
        }
    }

    inline int GetJsonInt(std::string_view json, std::string_view key, int fallback = 0) {
        return static_cast<int>(GetJsonFloat(json, key, static_cast<float>(fallback)));
    }

    inline bool GetJsonBool(std::string_view json, std::string_view key, bool fallback = false) {
        std::string search = "\"";
        search += key;
        search += "\":";
        auto pos = json.find(search);
        if (pos == std::string_view::npos) {
            return fallback;
        }
        pos += search.size();
        auto sub = json.substr(pos);
        if (sub.starts_with("true")) {
            return true;
        }
        if (sub.starts_with("false")) {
            return false;
        }
        return fallback;
    }
}
