#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/String.h>
#pragma warning(pop)

#include "PrismaUI/Core.h"

#include <functional>
#include <string>

namespace PrismaUI::Communication
{
    void Invoke(
        Core::PrismaViewId viewId,
        const ultralight::String& script,
        std::function<void(std::string)> callback = nullptr) noexcept;

    void InteropCall(
        Core::PrismaViewId viewId,
        const std::string& functionName,
        const std::string& argument) noexcept;

    void RegisterJSListener(
        Core::PrismaViewId viewId,
        const std::string& functionName,
        Core::SimpleJSCallback callback) noexcept;

    [[nodiscard]] bool BindJSCallbacks(
        Core::PrismaViewId viewId) noexcept;

    JSValueRef InvokeCppCallback(
        JSContextRef context,
        JSObjectRef function,
        JSObjectRef thisObject,
        std::size_t argumentCount,
        const JSValueRef arguments[],
        JSValueRef* exception) noexcept;
}
