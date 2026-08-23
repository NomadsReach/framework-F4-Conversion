#include "API.h"

#include "PrismaUI/Communication.h"
#include "PrismaUI/GameThreadDispatcher.h"
#include "PrismaUI/Translations.h"
#include "PrismaUI/ViewManager.h"
#include "Utils/Encoding.h"

namespace {

bool DispatchLegacyCallback(PrismaUI::Core::PrismaViewId viewId, std::function<void()> callback)
{
    if (!callback) return false;
    auto fallback = callback;
    if (PrismaUI::GameThreadDispatcher::Dispatch(std::move(callback), viewId)) return true;

    if (auto* tasks = F4SE::GetTaskInterface()) {
        tasks->AddTask(std::move(fallback));
        return true;
    }
    return false;
}

std::string NormalizeUtf8(const char* value)
{
    if (!value) return {};
    if (isValidUTF8(value)) return value;
    return convertFromANSIToUTF8(value);
}

}

PrismaView PluginAPI::PrismaUIInterface::CreateView(
    const char* htmlPath, PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback) noexcept
{
    if (!htmlPath) return 0;

    try {
        std::function<void(PrismaUI::Core::PrismaViewId)> wrapper;
        if (onDomReadyCallback) {
            wrapper = [onDomReadyCallback](PrismaUI::Core::PrismaViewId viewId) {
                if (!DispatchLegacyCallback(viewId, [onDomReadyCallback, viewId] { onDomReadyCallback(viewId); })) {
                    logger::error("OnDomReady dispatch failed for View [{}]", viewId);
                }
            };
        }
        return PrismaUI::ViewManager::Create(htmlPath, std::move(wrapper));
    } catch (const std::exception& e) {
        logger::error("CreateView failed: {}", e.what());
        return 0;
    } catch (...) {
        logger::error("CreateView failed");
        return 0;
    }
}

void PluginAPI::PrismaUIInterface::Invoke(PrismaView view, const char* script,
                                          PRISMA_UI_API::JSCallback callback) noexcept
{
    if (!view || !script) return;

    try {
        const std::string normalized = NormalizeUtf8(script);
        if (normalized.empty() && script[0] != '\0') return;

        std::function<void(std::string)> wrapper;
        if (callback) {
            wrapper = [callback, view](std::string result) {
                if (!DispatchLegacyCallback(view, [callback, result = std::move(result)] { callback(result.c_str()); })) {
                    logger::error("Invoke callback dispatch failed for View [{}]", view);
                }
            };
        }

        PrismaUI::Communication::Invoke(view, ultralight::String(normalized.c_str()), std::move(wrapper));
    } catch (const std::exception& e) {
        logger::error("Invoke failed for View [{}]: {}", view, e.what());
    } catch (...) {
        logger::error("Invoke failed for View [{}]", view);
    }
}

void PluginAPI::PrismaUIInterface::InteropCall(PrismaView view, const char* functionName,
                                                const char* argument) noexcept
{
    if (!view || !functionName || !argument) return;

    try {
        const std::string normalized = NormalizeUtf8(argument);
        if (normalized.empty() && argument[0] != '\0') return;
        PrismaUI::Communication::InteropCall(view, functionName, normalized);
    } catch (const std::exception& e) {
        logger::error("InteropCall failed for View [{}]: {}", view, e.what());
    } catch (...) {
        logger::error("InteropCall failed for View [{}]", view);
    }
}

void PluginAPI::PrismaUIInterface::RegisterJSListener(PrismaView view, const char* functionName,
                                                       PRISMA_UI_API::JSListenerCallback callback) noexcept
{
    if (!view || !functionName || !functionName[0] || !callback) return;

    try {
        PrismaUI::Communication::RegisterJSListener(view, functionName,
            [callback, view](std::string argument) {
                if (!DispatchLegacyCallback(view,
                        [callback, argument = std::move(argument)] { callback(argument.c_str()); })) {
                    logger::error("RegisterJSListener callback dispatch failed for View [{}]", view);
                }
            });
    } catch (const std::exception& e) {
        logger::error("RegisterJSListener failed for View [{}]: {}", view, e.what());
    } catch (...) {
        logger::error("RegisterJSListener failed for View [{}]", view);
    }
}

bool PluginAPI::PrismaUIInterface::HasFocus(PrismaView view) noexcept
{
    return view && PrismaUI::ViewManager::HasFocus(view);
}

bool PluginAPI::PrismaUIInterface::Focus(PrismaView view, bool pauseGame, bool disableFocusMenu) noexcept
{
    return view && PrismaUI::ViewManager::Focus(view, pauseGame, disableFocusMenu);
}

void PluginAPI::PrismaUIInterface::Unfocus(PrismaView view) noexcept
{
    if (view) PrismaUI::ViewManager::Unfocus(view);
}

void PluginAPI::PrismaUIInterface::Show(PrismaView view) noexcept
{
    if (view) PrismaUI::ViewManager::Show(view);
}

void PluginAPI::PrismaUIInterface::Hide(PrismaView view) noexcept
{
    if (view) PrismaUI::ViewManager::Hide(view);
}

bool PluginAPI::PrismaUIInterface::IsHidden(PrismaView view) noexcept
{
    return !view || PrismaUI::ViewManager::IsHidden(view);
}

int PluginAPI::PrismaUIInterface::GetScrollingPixelSize(PrismaView view) noexcept
{
    return view ? PrismaUI::ViewManager::GetScrollingPixelSize(view) : 0;
}

void PluginAPI::PrismaUIInterface::SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept
{
    if (view) PrismaUI::ViewManager::SetScrollingPixelSize(view, pixelSize);
}

bool PluginAPI::PrismaUIInterface::IsValid(PrismaView view) noexcept
{
    return view && PrismaUI::ViewManager::IsValid(view);
}

void PluginAPI::PrismaUIInterface::Destroy(PrismaView view) noexcept
{
    if (view) PrismaUI::ViewManager::Destroy(view);
}

void PluginAPI::PrismaUIInterface::SetOrder(PrismaView view, int order) noexcept
{
    if (view) PrismaUI::ViewManager::SetOrder(view, order);
}

int PluginAPI::PrismaUIInterface::GetOrder(PrismaView view) noexcept
{
    return view ? PrismaUI::ViewManager::GetOrder(view) : -1;
}

void PluginAPI::PrismaUIInterface::CreateInspectorView(PrismaView view) noexcept
{
    if (view) PrismaUI::ViewManager::CreateInspectorView(view);
}

void PluginAPI::PrismaUIInterface::SetInspectorVisibility(PrismaView view, bool visible) noexcept
{
    if (view) PrismaUI::ViewManager::SetInspectorVisibility(view, visible);
}

bool PluginAPI::PrismaUIInterface::IsInspectorVisible(PrismaView view) noexcept
{
    return view && PrismaUI::ViewManager::IsInspectorVisible(view);
}

void PluginAPI::PrismaUIInterface::SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY,
                                                       unsigned int width, unsigned int height) noexcept
{
    if (view) PrismaUI::ViewManager::SetInspectorBounds(view, topLeftX, topLeftY, width, height);
}

bool PluginAPI::PrismaUIInterface::HasAnyActiveFocus() noexcept
{
    return PrismaUI::ViewManager::HasAnyActiveFocus();
}

void PluginAPI::PrismaUIInterface::RegisterConsoleCallback(
    PrismaView view, PRISMA_UI_API::ConsoleMessageCallback callback) noexcept
{
    if (!view) return;

    if (!callback) {
        PrismaUI::ViewManager::RegisterConsoleCallback(view, nullptr);
        return;
    }

    PrismaUI::ViewManager::RegisterConsoleCallback(view,
        [callback](PrismaUI::Core::PrismaViewId id, PRISMA_UI_API::ConsoleMessageLevel level,
                   const std::string& message) {
            if (!DispatchLegacyCallback(id, [callback, id, level, message] { callback(id, level, message.c_str()); })) {
                logger::error("Console callback dispatch failed for View [{}]", id);
            }
        });
}

void PluginAPI::PrismaUIInterface::RegisterTranslations(PrismaView view, const char* pluginName) noexcept
{
    if (view && pluginName && pluginName[0]) PrismaUI::ViewManager::RegisterTranslations(view, pluginName);
}

void PluginAPI::PrismaUIInterface::BindUIEvent(PrismaView view, const char* functionName,
                                                PRISMA_UI_API::JSListenerCallback callback) noexcept
{
    if (!view || !functionName || !functionName[0] || !callback) return;
    if (!PrismaUI::GameThreadDispatcher::IsReady()) {
        logger::error("BindUIEvent refused for View [{}]: verified game-thread dispatcher is not ready", view);
        return;
    }

    PrismaUI::Communication::RegisterJSListener(view, functionName,
        [callback, view](std::string argument) {
            if (!PrismaUI::GameThreadDispatcher::Dispatch(
                    [callback, argument = std::move(argument)] { callback(argument.c_str()); }, view)) {
                logger::error("BindUIEvent callback dispatch failed for View [{}]", view);
            }
        });
}

void PluginAPI::PrismaUIInterface::EnumerateViews(PRISMA_UI_API::ViewEnumCallback callback,
                                                   void* userdata) noexcept
{
    if (!callback) return;
    PrismaUI::ViewManager::EnumerateViews(
        [callback, userdata](PrismaUI::Core::PrismaViewId id, const std::string& path) {
            callback(static_cast<PrismaView>(id), path.c_str(), userdata);
        });
}
