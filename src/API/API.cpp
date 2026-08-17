#include "API.h"

#include "PrismaUI/Communication.h"
#include "PrismaUI/Translations.h"
#include "PrismaUI/ViewManager.h"
#include "Utils/Encoding.h"

PrismaView PluginAPI::PrismaUIInterface::CreateView(
    const char* htmlPath, PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback) noexcept {
    if (!htmlPath) {
        return 0;
    }

    std::function<void(PrismaUI::Core::PrismaViewId)> domReadyWrapper = nullptr;
    if (onDomReadyCallback) {
        domReadyWrapper = [onDomReadyCallback](PrismaUI::Core::PrismaViewId viewId) {
            logger::info("CreateView: DOM ready for view [{}] - dispatching OnDomReady to game thread", viewId);
            F4SE::GetTaskInterface()->AddTask([callback = onDomReadyCallback, id = viewId]() {
                logger::info("CreateView: OnDomReady fired for view [{}] on game thread", id);
                callback(id);
            });
        };
    }

    auto newViewId = PrismaUI::ViewManager::Create(htmlPath, std::move(domReadyWrapper));
    logger::info("CreateView: path='{}' -> view [{}]", htmlPath, newViewId);
    return newViewId;
}

void PluginAPI::PrismaUIInterface::Invoke(PrismaView view, const char* script,
                                           PRISMA_UI_API::JSCallback callback) noexcept {
    if (!view || !script) {
        return;
    }

    std::string processedScript;
    if (isValidUTF8(script)) {
        processedScript = script;
    } else {
        processedScript = convertFromANSIToUTF8(script);
        if (processedScript.empty()) {
            return;
        }
    }

    ultralight::String ulScript(processedScript.c_str());
    std::function<void(std::string)> callbackWrapper = nullptr;
    if (callback) {
        callbackWrapper = [callback](const std::string& result) {
            F4SE::GetTaskInterface()->AddTask([targetCallback = callback, data = result]() {
                targetCallback(data.c_str());
            });
        };
    }

    PrismaUI::Communication::Invoke(view, ulScript, std::move(callbackWrapper));
}

void PluginAPI::PrismaUIInterface::InteropCall(PrismaView view, const char* functionName,
                                                const char* argument) noexcept {
    if (!view || !functionName || !argument) {
        return;
    }

    std::string processedArgument;
    if (isValidUTF8(argument)) {
        processedArgument = argument;
    } else {
        processedArgument = convertFromANSIToUTF8(argument);
        if (processedArgument.empty()) {
            return;
        }
    }

    PrismaUI::Communication::InteropCall(view, functionName, processedArgument);
}

void PluginAPI::PrismaUIInterface::RegisterJSListener(PrismaView view, const char* fnName,
                                                       PRISMA_UI_API::JSListenerCallback callback) noexcept {
    if (!view || !fnName || !callback) {
        logger::warn("RegisterJSListener: invalid args - view={} fn={}", view, fnName ? fnName : "null");
        return;
    }

    logger::info("RegisterJSListener: View [{}] registering '{}'", view, fnName);
    std::string name = fnName;
    std::function<void(std::string)> callbackWrapper = [callback, view, name](const std::string& arg) {
        F4SE::GetTaskInterface()->AddTask([targetCallback = callback, data = arg, view, name]() {
            logger::info("RegisterJSListener fired: View [{}] '{}' - data: '{}'", view, name, data);
            targetCallback(data.c_str());
        });
    };

    PrismaUI::Communication::RegisterJSListener(view, fnName, std::move(callbackWrapper));
}

bool PluginAPI::PrismaUIInterface::HasFocus(PrismaView view) noexcept {
    return view && PrismaUI::ViewManager::HasFocus(view);
}

bool PluginAPI::PrismaUIInterface::Focus(PrismaView view, bool pauseGame, bool disableFocusMenu) noexcept {
    return view && PrismaUI::ViewManager::Focus(view, pauseGame, disableFocusMenu);
}

void PluginAPI::PrismaUIInterface::Unfocus(PrismaView view) noexcept {
    if (view) {
        PrismaUI::ViewManager::Unfocus(view);
    }
}

void PluginAPI::PrismaUIInterface::Show(PrismaView view) noexcept {
    if (view) {
        PrismaUI::ViewManager::Show(view);
    }
}

void PluginAPI::PrismaUIInterface::Hide(PrismaView view) noexcept {
    if (view) {
        PrismaUI::ViewManager::Hide(view);
    }
}

bool PluginAPI::PrismaUIInterface::IsHidden(PrismaView view) noexcept {
    return !view || PrismaUI::ViewManager::IsHidden(view);
}

int PluginAPI::PrismaUIInterface::GetScrollingPixelSize(PrismaView view) noexcept {
    return view ? PrismaUI::ViewManager::GetScrollingPixelSize(view) : 0;
}

void PluginAPI::PrismaUIInterface::SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept {
    if (view) {
        PrismaUI::ViewManager::SetScrollingPixelSize(view, pixelSize);
    }
}

bool PluginAPI::PrismaUIInterface::IsValid(PrismaView view) noexcept {
    return view && PrismaUI::ViewManager::IsValid(view);
}

void PluginAPI::PrismaUIInterface::Destroy(PrismaView view) noexcept {
    if (view) {
        PrismaUI::ViewManager::Destroy(view);
    }
}

void PluginAPI::PrismaUIInterface::SetOrder(PrismaView view, int order) noexcept {
    if (view) {
        PrismaUI::ViewManager::SetOrder(view, order);
    }
}

int PluginAPI::PrismaUIInterface::GetOrder(PrismaView view) noexcept {
    return view ? PrismaUI::ViewManager::GetOrder(view) : -1;
}

void PluginAPI::PrismaUIInterface::CreateInspectorView(PrismaView view) noexcept {
    if (view) {
        PrismaUI::ViewManager::CreateInspectorView(view);
    }
}

void PluginAPI::PrismaUIInterface::SetInspectorVisibility(PrismaView view, bool visible) noexcept {
    if (view) {
        PrismaUI::ViewManager::SetInspectorVisibility(view, visible);
    }
}

bool PluginAPI::PrismaUIInterface::IsInspectorVisible(PrismaView view) noexcept {
    return view && PrismaUI::ViewManager::IsInspectorVisible(view);
}

void PluginAPI::PrismaUIInterface::SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY,
                                                       unsigned int width, unsigned int height) noexcept {
    if (view) {
        PrismaUI::ViewManager::SetInspectorBounds(view, topLeftX, topLeftY, width, height);
    }
}

bool PluginAPI::PrismaUIInterface::HasAnyActiveFocus() noexcept {
    return PrismaUI::ViewManager::HasAnyActiveFocus();
}

void PluginAPI::PrismaUIInterface::RegisterConsoleCallback(
    PrismaView view, PRISMA_UI_API::ConsoleMessageCallback callback) noexcept {
    if (!view) {
        return;
    }

    if (callback) {
        logger::info("RegisterConsoleCallback: View [{}] registered JS console capture", view);
        auto wrappedCallback = [callback](PrismaUI::Core::PrismaViewId id, PRISMA_UI_API::ConsoleMessageLevel level,
                                          const std::string& msg) {
            F4SE::GetTaskInterface()->AddTask([callback, id, level, msg]() { callback(id, level, msg.c_str()); });
        };
        PrismaUI::ViewManager::RegisterConsoleCallback(view, std::move(wrappedCallback));
    } else {
        logger::info("RegisterConsoleCallback: View [{}] unregistered JS console capture", view);
        PrismaUI::ViewManager::RegisterConsoleCallback(view, nullptr);
    }
}

void PluginAPI::PrismaUIInterface::RegisterTranslations(PrismaView view, const char* pluginName) noexcept {
    if (!view || !pluginName || pluginName[0] == '\0') {
        logger::warn("[V3] RegisterTranslations: invalid args - view={}", view);
        return;
    }
    logger::info("[V3] RegisterTranslations: View [{}] -> plugin '{}'", view, pluginName);
    PrismaUI::ViewManager::RegisterTranslations(view, pluginName);
}

void PluginAPI::PrismaUIInterface::BindUIEvent(PrismaView view, const char* functionName,
                                                PRISMA_UI_API::JSListenerCallback callback) noexcept {
    if (!view || !functionName || !callback) {
        logger::warn("[V4] BindUIEvent: invalid args - view={} fn={}", view, functionName ? functionName : "null");
        return;
    }

    logger::info("[V4] BindUIEvent: View [{}] registering game-thread listener '{}'", view, functionName);
    std::string fnName = functionName;
    auto wrapped = [callback, view, fnName](const std::string& arg) {
        F4SE::GetTaskInterface()->AddTask([callback, view, fnName, arg]() {
            logger::info("[V4] BindUIEvent fired: View [{}] '{}' - data: '{}'", view, fnName, arg);
            callback(arg.c_str());
        });
    };

    PrismaUI::Communication::RegisterJSListener(view, functionName, std::move(wrapped));
}

void PluginAPI::PrismaUIInterface::EnumerateViews(PRISMA_UI_API::ViewEnumCallback callback,
                                                   void* userdata) noexcept {
    if (!callback) {
        return;
    }
    PrismaUI::ViewManager::EnumerateViews(
        [callback, userdata](PrismaUI::Core::PrismaViewId id, const std::string& path) {
            callback(static_cast<PrismaView>(id), path.c_str(), userdata);
        });
}
