#include "Listeners.h"

#include "Communication.h"
#include "Core.h"
#include "NetworkSandbox.h"
#include "PapyrusBridge.h"
#include "PrismaUI_F4_API.h"
#include "Translations.h"

namespace PrismaUI::Listeners {
using namespace Communication;
using namespace Core;

MyLoadListener::MyLoadListener(Core::PrismaViewId id) : viewId_(id) {}
MyLoadListener::~MyLoadListener() = default;

void MyLoadListener::OnBeginLoading(View*, uint64_t, bool isMainFrame, const String& url)
{
    if (isMainFrame) logger::info("View [{}]: loading {}", viewId_, url.utf8().data());
}

void MyLoadListener::OnFinishLoading(View*, uint64_t, bool isMainFrame, const String& url)
{
    if (!isMainFrame) return;

    std::shared_ptr<PrismaView> viewData;
    {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it != views.end()) viewData = it->second;
    }
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) return;

    viewData->lastLoadedUrl = url.utf8().data();
    viewData->recoveryAttempts.store(0, std::memory_order_release);
    viewData->isLoadingFinished.store(true, std::memory_order_release);
    Communication::BindJSCallbacks(viewId_);
    logger::info("View [{}]: load complete", viewId_);
}

void MyLoadListener::OnFailLoading(View*, uint64_t, bool isMainFrame, const String& url,
                                   const String& description, const String&, int)
{
    if (!isMainFrame) return;

    std::shared_ptr<PrismaView> viewData;
    {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it != views.end()) viewData = it->second;
    }
    if (viewData && !viewData->isDestroying.load(std::memory_order_acquire)) {
        viewData->isLoadingFinished.store(false, std::memory_order_release);
    }

    logger::error("View [{}]: failed loading {}: {}", viewId_, url.utf8().data(), description.utf8().data());
}

void MyLoadListener::OnWindowObjectReady(View* caller, uint64_t, bool isMainFrame, const String&)
{
    if (!isMainFrame || !caller) return;

    std::string pluginName;
    {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) return;
        pluginName = it->second->translationPluginName;
    }

    PapyrusBridge::InjectBridge(caller, viewId_);

    ultralight::String exception;
    caller->EvaluateScript(ultralight::String(NetworkSandbox::kNetworkBlockScript), &exception);
    if (!exception.empty()) {
        logger::warn("View [{}]: network sandbox injection failed: {}", viewId_, exception.utf8().data());
    }

    if (pluginName.empty()) return;

    const auto language = Translations::DetectGameLanguage();
    const auto translations = Translations::ParseTranslationFile(pluginName, language);
    const auto script = Translations::BuildL10NScript(translations);
    if (script.empty()) return;

    caller->EvaluateScript(ultralight::String(script.c_str()));
}

void MyLoadListener::OnDOMReady(View*, uint64_t, bool isMainFrame, const String&)
{
    if (!isMainFrame) return;

    std::function<void(Core::PrismaViewId)> callback;
    {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) return;
        callback = it->second->domReadyCallback;
    }

    if (callback) callback(viewId_);
}

MyViewListener::MyViewListener(Core::PrismaViewId id) : viewId_(id) {}
MyViewListener::~MyViewListener() = default;

RefPtr<View> MyViewListener::OnCreateChildView(View*, const String&, const String& targetUrl, bool, const IntRect&)
{
    logger::warn("[PrismaUI Security] blocked child view navigation to '{}'", targetUrl.utf8().data());
    return nullptr;
}

void MyViewListener::OnAddConsoleMessage(View*, const ultralight::ConsoleMessage& message)
{
    if (message.source() == kMessageSource_Network) {
        logger::warn("[PrismaUI Security] View [{}]: [Network] {}", viewId_, message.message().utf8().data());
    }

    std::function<void(Core::PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback;
    {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) return;
        callback = it->second->consoleMessageCallback;
    }
    if (!callback) return;

    PRISMA_UI_API::ConsoleMessageLevel level = PRISMA_UI_API::ConsoleMessageLevel::Log;
    switch (message.level()) {
        case kMessageLevel_Warning: level = PRISMA_UI_API::ConsoleMessageLevel::Warning; break;
        case kMessageLevel_Error: level = PRISMA_UI_API::ConsoleMessageLevel::Error; break;
        case kMessageLevel_Debug: level = PRISMA_UI_API::ConsoleMessageLevel::Debug; break;
        case kMessageLevel_Info: level = PRISMA_UI_API::ConsoleMessageLevel::Info; break;
        default: break;
    }

    callback(viewId_, level, message.message().utf8().data());
}

RefPtr<View> MyViewListener::OnCreateInspectorView(View*, bool, const String&)
{
    std::unique_lock lock(viewsMutex);
    auto it = views.find(viewId_);
    if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) return nullptr;

    auto& viewData = it->second;
    if (viewData->inspectorView) return viewData->inspectorView;
    if (!viewData->ultralightView || !renderer) return nullptr;

    const uint32_t width = viewData->inspectorDisplayWidth > 0 ? viewData->inspectorDisplayWidth : 800;
    const uint32_t height = viewData->inspectorDisplayHeight > 0 ? viewData->inspectorDisplayHeight : 600;

    ViewConfig config;
    config.is_accelerated = false;
    config.is_transparent = true;
    viewData->inspectorView = renderer->CreateView(width, height, config, nullptr);
    return viewData->inspectorView;
}

MyUltralightLogger::~MyUltralightLogger() = default;
void MyUltralightLogger::LogMessage(LogLevel, const String&) {}

}
