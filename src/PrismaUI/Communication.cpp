#include "Communication.h"

#include <utility>
#include <vector>

#include "Core.h"
#include "Translations.h"
#include "ViewManager.h"

namespace PrismaUI::Communication {
using namespace Core;

namespace {

std::shared_ptr<PrismaView> GetLiveView(Core::PrismaViewId viewId)
{
    std::shared_lock lock(viewsMutex);
    auto it = views.find(viewId);
    if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) return nullptr;
    return it->second;
}

template <class F>
void RunOnUltralight(SingleThreadExecutor::Priority priority, F&& fn)
{
    if (ultralightThread.IsWorkerThread()) {
        std::forward<F>(fn)();
        return;
    }

    try {
        ultralightThread.submit_with_priority(priority, std::forward<F>(fn));
    } catch (const std::exception& e) {
        logger::error("Ultralight dispatch failed: {}", e.what());
    }
}

std::string JSStringToUtf8(JSStringRef value)
{
    if (!value) return {};
    const size_t size = JSStringGetMaximumUTF8CStringSize(value);
    if (size == 0) return {};
    std::vector<char> buffer(size, '\0');
    JSStringGetUTF8CString(value, buffer.data(), buffer.size());
    return buffer.data();
}

std::string JSValueToUtf8(JSContextRef ctx, JSValueRef value, JSValueRef* exception)
{
    if (!value) return {};
    JSStringRef string = JSValueToStringCopy(ctx, value, exception);
    if (!string) return {};
    std::string result = JSStringToUtf8(string);
    JSStringRelease(string);
    return result;
}

std::string ExceptionText(JSContextRef ctx, JSValueRef exception)
{
    if (!exception) return {};
    return JSValueToUtf8(ctx, exception, nullptr);
}

}

void Invoke(const Core::PrismaViewId& viewId, const String& script, std::function<void(std::string)> callback)
{
    auto viewData = GetLiveView(viewId);
    if (!viewData) {
        if (callback) callback({});
        return;
    }

    RunOnUltralight(SingleThreadExecutor::Priority::MEDIUM,
        [viewData, scriptCopy = script, callback = std::move(callback)]() mutable {
            if (viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) {
                if (callback) callback({});
                return;
            }

            String result;
            try {
                result = viewData->ultralightView->EvaluateScript(scriptCopy, nullptr);
            } catch (const std::exception& e) {
                logger::error("EvaluateScript failed for View [{}]: {}", viewData->id, e.what());
            } catch (...) {
                logger::error("EvaluateScript failed for View [{}]", viewData->id);
            }

            if (callback) callback(result.utf8().data());
        });
}

void RegisterJSListener(const Core::PrismaViewId& viewId, const std::string& name, Core::SimpleJSCallback callback)
{
    if (name.empty() || !callback || !ViewManager::IsValid(viewId)) return;

    {
        std::lock_guard lock(jsCallbacksMutex);
        jsCallbacks[{viewId, name}] = JSCallbackData{viewId, name, std::move(callback)};
    }

    auto viewData = GetLiveView(viewId);
    if (!viewData || !viewData->ultralightView || !viewData->isLoadingFinished.load(std::memory_order_acquire)) return;

    RunOnUltralight(SingleThreadExecutor::Priority::HIGH, [viewId]() { BindJSCallbacks(viewId); });
}

void BindJSCallbacks(const Core::PrismaViewId& viewId)
{
    if (!ultralightThread.IsWorkerThread()) {
        RunOnUltralight(SingleThreadExecutor::Priority::HIGH, [viewId]() { BindJSCallbacks(viewId); });
        return;
    }

    std::shared_ptr<PrismaView> viewData;
    std::unordered_map<std::string, std::string> translations;
    {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) return;
        viewData = it->second;
        translations = viewData->translations;
    }

    if (!viewData->ultralightView || !viewData->isLoadingFinished.load(std::memory_order_acquire)) return;

    if (!translations.empty()) {
        const std::string script = Translations::BuildL10NScript(translations);
        if (!script.empty()) {
            try {
                viewData->ultralightView->EvaluateScript(ultralight::String(script.c_str()), nullptr);
            } catch (...) {
                logger::error("Translation injection failed for View [{}]", viewId);
            }
        }
    }

    std::vector<JSCallbackData> callbacks;
    {
        std::lock_guard lock(jsCallbacksMutex);
        for (const auto& [key, data] : jsCallbacks) {
            if (key.first == viewId) callbacks.push_back(data);
        }
    }
    if (callbacks.empty() || viewData->isDestroying.load(std::memory_order_acquire)) return;

    auto scopedContext = viewData->ultralightView->LockJSContext();
    JSContextRef ctx = scopedContext->ctx();
    JSObjectRef global = JSContextGetGlobalObject(ctx);

    for (const auto& callbackData : callbacks) {
        if (viewData->isDestroying.load(std::memory_order_acquire)) return;

        JSObjectRef dataObject = JSObjectMake(ctx, nullptr, nullptr);
        JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
        JSStringRef nameKey = JSStringCreateWithUTF8CString("name");
        JSStringRef viewIdValue = JSStringCreateWithUTF8CString(std::to_string(callbackData.viewId).c_str());
        JSStringRef nameValue = JSStringCreateWithUTF8CString(callbackData.name.c_str());

        JSObjectSetProperty(ctx, dataObject, viewIdKey, JSValueMakeString(ctx, viewIdValue),
                            kJSPropertyAttributeReadOnly, nullptr);
        JSObjectSetProperty(ctx, dataObject, nameKey, JSValueMakeString(ctx, nameValue),
                            kJSPropertyAttributeReadOnly, nullptr);

        JSStringRelease(viewIdKey);
        JSStringRelease(nameKey);
        JSStringRelease(viewIdValue);
        JSStringRelease(nameValue);

        JSStringRef functionName = JSStringCreateWithUTF8CString(callbackData.name.c_str());
        JSObjectRef function = JSObjectMakeFunctionWithCallback(ctx, functionName, InvokeCppCallback);
        JSStringRef dataKey = JSStringCreateWithUTF8CString("data");
        JSObjectSetProperty(ctx, function, dataKey, dataObject, kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(dataKey);
        JSObjectSetProperty(ctx, global, functionName, function, kJSPropertyAttributeNone, nullptr);
        JSStringRelease(functionName);
    }
}

void InteropCall(const Core::PrismaViewId& viewId, const std::string& functionName, const std::string& argument)
{
    auto viewData = GetLiveView(viewId);
    if (!viewData || !viewData->ultralightView) return;

    RunOnUltralight(SingleThreadExecutor::Priority::MEDIUM,
        [viewData, functionName, argument]() {
            if (viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) return;

            auto scopedContext = viewData->ultralightView->LockJSContext();
            JSContextRef ctx = scopedContext->ctx();
            JSObjectRef global = JSContextGetGlobalObject(ctx);
            JSValueRef exception = nullptr;

            JSRetainPtr<JSStringRef> name = adopt(JSStringCreateWithUTF8CString(functionName.c_str()));
            JSValueRef value = JSObjectGetProperty(ctx, global, name.get(), &exception);
            if (exception) {
                logger::error("InteropCall [{}] '{}': {}", viewData->id, functionName, ExceptionText(ctx, exception));
                return;
            }
            if (!value || !JSValueIsObject(ctx, value)) return;

            JSObjectRef function = JSValueToObject(ctx, value, &exception);
            if (exception || !function || !JSObjectIsFunction(ctx, function)) return;

            JSRetainPtr<JSStringRef> arg = adopt(JSStringCreateWithUTF8CString(argument.c_str()));
            const JSValueRef args[] = {JSValueMakeString(ctx, arg.get())};
            JSObjectCallAsFunction(ctx, function, global, 1, args, &exception);
            if (exception) {
                logger::error("InteropCall [{}] '{}': {}", viewData->id, functionName, ExceptionText(ctx, exception));
            }
        });
}

JSValueRef InvokeCppCallback(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argumentCount,
                             const JSValueRef arguments[], JSValueRef* exception)
{
    JSStringRef dataKey = JSStringCreateWithUTF8CString("data");
    JSValueRef dataValue = JSObjectGetProperty(ctx, function, dataKey, exception);
    JSStringRelease(dataKey);
    if (!dataValue || JSValueIsNull(ctx, dataValue) || JSValueIsUndefined(ctx, dataValue)) {
        return JSValueMakeUndefined(ctx);
    }

    JSObjectRef dataObject = JSValueToObject(ctx, dataValue, exception);
    if (!dataObject) return JSValueMakeUndefined(ctx);

    JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
    JSStringRef nameKey = JSStringCreateWithUTF8CString("name");
    JSValueRef viewIdValue = JSObjectGetProperty(ctx, dataObject, viewIdKey, exception);
    JSValueRef nameValue = JSObjectGetProperty(ctx, dataObject, nameKey, exception);
    JSStringRelease(viewIdKey);
    JSStringRelease(nameKey);
    if (!viewIdValue || !nameValue) return JSValueMakeUndefined(ctx);

    const std::string viewIdText = JSValueToUtf8(ctx, viewIdValue, exception);
    const std::string name = JSValueToUtf8(ctx, nameValue, exception);
    if (viewIdText.empty() || name.empty()) return JSValueMakeUndefined(ctx);

    Core::PrismaViewId viewId = 0;
    try {
        viewId = std::stoull(viewIdText);
    } catch (...) {
        return JSValueMakeUndefined(ctx);
    }

    if (!ViewManager::IsValid(viewId)) return JSValueMakeUndefined(ctx);

    std::string argument;
    if (argumentCount > 0) argument = JSValueToUtf8(ctx, arguments[0], exception);

    Core::SimpleJSCallback callback;
    {
        std::lock_guard lock(jsCallbacksMutex);
        auto it = jsCallbacks.find({viewId, name});
        if (it != jsCallbacks.end()) callback = it->second.callback;
    }
    if (!callback) return JSValueMakeUndefined(ctx);

    try {
        callback(argument);
    } catch (const std::exception& e) {
        logger::error("JS callback '{}' for View [{}] threw: {}", name, viewId, e.what());
    } catch (...) {
        logger::error("JS callback '{}' for View [{}] threw", name, viewId);
    }

    return JSValueMakeUndefined(ctx);
}

}
