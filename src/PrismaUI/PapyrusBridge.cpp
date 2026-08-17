#include "PapyrusBridge.h"

#include <cctype>
#include <cstring>

#include "Communication.h"
#include "GameThreadDispatcher.h"
#include "ViewManager.h"

#pragma warning(push)
#pragma warning(disable : 4100)
#include <JavaScriptCore/JSBase.h>
#include <JavaScriptCore/JSObjectRef.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <JavaScriptCore/JSStringRef.h>
#include <JavaScriptCore/JSValueRef.h>
#pragma warning(pop)

namespace PrismaUI::PapyrusBridge {
namespace {

constexpr const char* kBridgeScript = R"js(
(function() {
    'use strict';
    var pending = {};
    var nextId = 1;

    window.__prisma_resolve = function(id, value) {
        var callback = pending[id];
        if (!callback) return;
        delete pending[id];
        callback.resolve(value);
    };

    window.__prisma_reject = function(id, message) {
        var callback = pending[id];
        if (!callback) return;
        delete pending[id];
        callback.reject(new Error(String(message)));
    };

    function read(op, params) {
        return new Promise(function(resolve, reject) {
            var id = String(nextId++);
            pending[id] = { resolve: resolve, reject: reject };
            try {
                window.__prisma_request(JSON.stringify(Object.assign({ op: op, id: id }, params)));
            } catch (error) {
                delete pending[id];
                reject(error);
            }
        });
    }

    function write(op, params) {
        try {
            window.__prisma_request(JSON.stringify(Object.assign({ op: op, id: '' }, params)));
        } catch (error) {}
    }

    window.prisma = Object.freeze({
        getGlobal: function(esp, formId) {
            return read('getGlobal', { esp: String(esp), formId: String(formId) });
        },
        setGlobal: function(esp, formId, value) {
            write('setGlobal', { esp: String(esp), formId: String(formId), value: +value });
        },
        getProperty: function(esp, formId, scriptName, propName) {
            return read('getProperty', {
                esp: String(esp),
                formId: String(formId),
                scriptName: String(scriptName),
                propName: String(propName)
            });
        },
        setProperty: function(esp, formId, scriptName, propName, value) {
            write('setProperty', {
                esp: String(esp),
                formId: String(formId),
                scriptName: String(scriptName),
                propName: String(propName),
                value: value
            });
        }
    });
})();
)js";

struct PropResult {
    bool found = false;
    bool isBool = false;
    bool boolValue = false;
    double numberValue = 0.0;
};

std::string JsonGetString(const std::string& json, const std::string& key)
{
    const std::string prefix = "\"" + key + "\":\"";
    size_t pos = json.find(prefix);
    if (pos == std::string::npos) return {};
    pos += prefix.size();

    std::string result;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) {
            switch (c) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: return {};
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') return result;
        result.push_back(c);
    }
    return {};
}

double JsonGetNumberOrBool(const std::string& json, const std::string& key)
{
    const std::string prefix = "\"" + key + "\":";
    size_t pos = json.find(prefix);
    if (pos == std::string::npos) return 0.0;
    pos += prefix.size();

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (json.compare(pos, 4, "true") == 0) return 1.0;
    if (json.compare(pos, 5, "false") == 0 || json.compare(pos, 4, "null") == 0) return 0.0;

    try {
        return std::stod(json.substr(pos));
    } catch (...) {
        return 0.0;
    }
}

RE::TESForm* LookupFormByPlugin(const std::string& plugin, const std::string& localIdText)
{
    uint32_t localId = 0;
    try {
        localId = std::stoul(localIdText, nullptr, 16);
    } catch (...) {
        return nullptr;
    }

    auto* data = RE::TESDataHandler::GetSingleton();
    if (!data) return nullptr;

    const RE::TESFile* file = data->LookupModByName(plugin.c_str());
    if (!file) return nullptr;

    uint32_t formId = 0;
    if (file->IsLight()) {
        formId = 0xFE000000u | (static_cast<uint32_t>(file->GetSmallFileCompileIndex()) << 12u) | (localId & 0xFFFu);
    } else {
        formId = (static_cast<uint32_t>(file->GetCompileIndex()) << 24u) | (localId & 0x00FFFFFFu);
    }
    return RE::TESForm::GetFormByID(formId);
}

bool MatchesScript(RE::BSScript::Object* object, const std::string& scriptName)
{
    if (!object) return false;
    if (scriptName.empty()) return true;
    auto* typeInfo = object->GetTypeInfo();
    return typeInfo && _stricmp(typeInfo->GetName(), scriptName.c_str()) == 0;
}

PropResult GetPapyrusProperty(RE::TESForm* form, const std::string& scriptName, const std::string& propertyName)
{
    if (!form) return {};
    auto* gameVM = RE::GameVM::GetSingleton();
    if (!gameVM) return {};
    auto* vm = gameVM->GetVM().get();
    if (!vm) return {};
    auto* concreteVM = static_cast<RE::BSScript::Internal::VirtualMachine*>(vm);

    RE::BSAutoLock lock(concreteVM->attachedScriptsLock);
    const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<uint32_t>(form->GetFormType()), form);
    auto it = concreteVM->attachedScripts.find(handle);
    if (it == concreteVM->attachedScripts.end()) return {};

    for (auto& attached : it->second) {
        auto* object = attached.get();
        if (!MatchesScript(object, scriptName)) continue;

        auto* property = object->GetProperty(RE::BSFixedString(propertyName.c_str()));
        if (!property) continue;

        PropResult result;
        if (property->is<bool>()) {
            result.found = true;
            result.isBool = true;
            result.boolValue = RE::BSScript::get<bool>(*property);
        } else if (property->is<float>()) {
            result.found = true;
            result.numberValue = static_cast<double>(RE::BSScript::get<float>(*property));
        } else if (property->is<std::int32_t>()) {
            result.found = true;
            result.numberValue = static_cast<double>(RE::BSScript::get<std::int32_t>(*property));
        }
        return result;
    }
    return {};
}

bool SetPapyrusProperty(RE::TESForm* form, const std::string& scriptName, const std::string& propertyName,
                        double value)
{
    if (!form) return false;
    auto* gameVM = RE::GameVM::GetSingleton();
    if (!gameVM) return false;
    auto* vm = gameVM->GetVM().get();
    if (!vm) return false;
    auto* concreteVM = static_cast<RE::BSScript::Internal::VirtualMachine*>(vm);

    RE::BSAutoLock lock(concreteVM->attachedScriptsLock);
    const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<uint32_t>(form->GetFormType()), form);
    auto it = concreteVM->attachedScripts.find(handle);
    if (it == concreteVM->attachedScripts.end()) return false;

    for (auto& attached : it->second) {
        auto* object = attached.get();
        if (!MatchesScript(object, scriptName)) continue;

        auto* property = object->GetProperty(RE::BSFixedString(propertyName.c_str()));
        if (!property) continue;

        if (property->is<float>()) *property = static_cast<float>(value);
        else if (property->is<std::int32_t>()) *property = static_cast<std::int32_t>(value);
        else if (property->is<bool>()) *property = value != 0.0;
        else return false;
        return true;
    }
    return false;
}

std::string BuildResolve(const std::string& id, const PropResult& result)
{
    if (!result.found) return "__prisma_resolve('" + id + "',null)";
    if (result.isBool) {
        return "__prisma_resolve('" + id + "'," + std::string(result.boolValue ? "true" : "false") + ")";
    }
    return "__prisma_resolve('" + id + "'," + std::to_string(result.numberValue) + ")";
}

void Reject(Core::PrismaViewId viewId, const std::string& id, const char* message)
{
    if (id.empty() || !ViewManager::IsValid(viewId)) return;
    const std::string script = "__prisma_reject('" + id + "','" + message + "')";
    Communication::Invoke(viewId, ultralight::String(script.c_str()), nullptr);
}

std::string JSValueToUtf8(JSContextRef ctx, JSValueRef value, JSValueRef* exception)
{
    JSStringRef string = JSValueToStringCopy(ctx, value, exception);
    if (!string) return {};
    const size_t size = JSStringGetMaximumUTF8CStringSize(string);
    std::string result(size, '\0');
    JSStringGetUTF8CString(string, result.data(), result.size());
    JSStringRelease(string);
    result.resize(std::strlen(result.c_str()));
    return result;
}

JSValueRef PrismaRequestCallback(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argumentCount,
                                 const JSValueRef arguments[], JSValueRef* exception)
{
    if (argumentCount == 0) return JSValueMakeUndefined(ctx);

    const std::string request = JSValueToUtf8(ctx, arguments[0], exception);
    if (request.empty()) return JSValueMakeUndefined(ctx);

    JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
    JSValueRef viewIdValue = JSObjectGetProperty(ctx, function, viewIdKey, nullptr);
    JSStringRelease(viewIdKey);
    const std::string viewIdText = JSValueToUtf8(ctx, viewIdValue, nullptr);

    Core::PrismaViewId viewId = 0;
    try {
        viewId = std::stoull(viewIdText);
    } catch (...) {
        return JSValueMakeUndefined(ctx);
    }
    if (!ViewManager::IsValid(viewId)) return JSValueMakeUndefined(ctx);

    const std::string op = JsonGetString(request, "op");
    const std::string callbackId = JsonGetString(request, "id");

    if (op == "getGlobal") {
        const std::string plugin = JsonGetString(request, "esp");
        const std::string formId = JsonGetString(request, "formId");
        if (!GameThreadDispatcher::Dispatch([viewId, callbackId, plugin, formId] {
                if (!ViewManager::IsValid(viewId)) return;
                auto* form = LookupFormByPlugin(plugin, formId);
                auto* global = form ? form->As<RE::TESGlobal>() : nullptr;
                const std::string script = global
                    ? "__prisma_resolve('" + callbackId + "'," + std::to_string(global->value) + ")"
                    : "__prisma_resolve('" + callbackId + "',null)";
                Communication::Invoke(viewId, ultralight::String(script.c_str()), nullptr);
            }, viewId)) {
            Reject(viewId, callbackId, "game thread unavailable");
        }
    } else if (op == "setGlobal") {
        const std::string plugin = JsonGetString(request, "esp");
        const std::string formId = JsonGetString(request, "formId");
        const double value = JsonGetNumberOrBool(request, "value");
        if (!GameThreadDispatcher::Dispatch([plugin, formId, value] {
                auto* form = LookupFormByPlugin(plugin, formId);
                if (auto* global = form ? form->As<RE::TESGlobal>() : nullptr) {
                    global->value = static_cast<float>(value);
                }
            }, viewId)) {
            logger::warn("PapyrusBridge setGlobal dispatch failed for View [{}]", viewId);
        }
    } else if (op == "getProperty") {
        const std::string plugin = JsonGetString(request, "esp");
        const std::string formId = JsonGetString(request, "formId");
        const std::string scriptName = JsonGetString(request, "scriptName");
        const std::string propertyName = JsonGetString(request, "propName");
        if (!GameThreadDispatcher::Dispatch([viewId, callbackId, plugin, formId, scriptName, propertyName] {
                if (!ViewManager::IsValid(viewId)) return;
                const auto result = GetPapyrusProperty(LookupFormByPlugin(plugin, formId), scriptName, propertyName);
                const std::string script = BuildResolve(callbackId, result);
                Communication::Invoke(viewId, ultralight::String(script.c_str()), nullptr);
            }, viewId)) {
            Reject(viewId, callbackId, "game thread unavailable");
        }
    } else if (op == "setProperty") {
        const std::string plugin = JsonGetString(request, "esp");
        const std::string formId = JsonGetString(request, "formId");
        const std::string scriptName = JsonGetString(request, "scriptName");
        const std::string propertyName = JsonGetString(request, "propName");
        const double value = JsonGetNumberOrBool(request, "value");
        if (!GameThreadDispatcher::Dispatch([plugin, formId, scriptName, propertyName, value] {
                SetPapyrusProperty(LookupFormByPlugin(plugin, formId), scriptName, propertyName, value);
            }, viewId)) {
            logger::warn("PapyrusBridge setProperty dispatch failed for View [{}]", viewId);
        }
    } else {
        Reject(viewId, callbackId, "unknown operation");
    }

    return JSValueMakeUndefined(ctx);
}

}

void InjectBridge(ultralight::View* caller, Core::PrismaViewId viewId)
{
    if (!caller || !ViewManager::IsValid(viewId)) return;

    auto scoped = caller->LockJSContext();
    JSContextRef ctx = scoped->ctx();
    JSObjectRef global = JSContextGetGlobalObject(ctx);

    JSStringRef functionName = JSStringCreateWithUTF8CString("__prisma_request");
    JSObjectRef function = JSObjectMakeFunctionWithCallback(ctx, functionName, PrismaRequestCallback);
    JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
    JSStringRef viewIdValue = JSStringCreateWithUTF8CString(std::to_string(viewId).c_str());
    JSObjectSetProperty(ctx, function, viewIdKey, JSValueMakeString(ctx, viewIdValue), kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(viewIdKey);
    JSStringRelease(viewIdValue);
    JSObjectSetProperty(ctx, global, functionName, function, kJSPropertyAttributeNone, nullptr);
    JSStringRelease(functionName);

    ultralight::String exception;
    caller->EvaluateScript(ultralight::String(kBridgeScript), &exception);
    if (!exception.empty()) {
        logger::warn("PapyrusBridge [{}] injection failed: {}", viewId, exception.utf8().data());
    }
}

}
