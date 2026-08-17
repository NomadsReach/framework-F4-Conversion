#include "Listeners.h"

#include "Communication.h"
#include "Core.h"
#include "NetworkSandbox.h"
#include "PapyrusBridge.h"
#include "PrismaUI_F4_API.h"
#include "Translations.h"

namespace PrismaUI::Listeners {
    using namespace Core;
    using namespace Communication;

    MyLoadListener::MyLoadListener(Core::PrismaViewId id) : viewId_(std::move(id)) {}

    MyLoadListener::~MyLoadListener() = default;

    void MyLoadListener::OnBeginLoading(View* /*caller*/, uint64_t /*frame_id*/, bool /*is_main_frame*/,
                                        const String& url) {
        logger::info("View [{}]: LoadListener: Begin loading URL: {}", viewId_, url.utf8().data());
    }

    void MyLoadListener::OnFinishLoading(View* /*caller*/, uint64_t /*frame_id*/, bool /*is_main_frame*/,
                                         const String& url) {
        logger::info("View [{}]: LoadListener: Finished loading URL: {}", viewId_, url.utf8().data());
        ultralightThread.submit([id = viewId_, urlStr = std::string(url.utf8().data())] {
            std::unique_lock lock(viewsMutex);
            auto it = views.find(id);
            if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) {
                return;
            }

            it->second->isLoadingFinished = true;
            it->second->lastLoadedUrl = urlStr;
            it->second->recoveryAttempts = 0;
            lock.unlock();
            Communication::BindJSCallbacks(id);
        });
    }

    void MyLoadListener::OnFailLoading(View* /*caller*/, uint64_t /*frame_id*/, bool /*is_main_frame*/,
                                       const String& url, const String& description, const String& /*error_domain*/,
                                       int /*error_code*/) {
        logger::error("View [{}]: LoadListener: Failed loading URL: {}. Error: {}", viewId_, url.utf8().data(),
                      description.utf8().data());
        ultralightThread.submit([id = viewId_] {
            std::unique_lock lock(viewsMutex);
            auto it = views.find(id);
            if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
                it->second->isLoadingFinished = false;
            }
        });
    }

    void MyLoadListener::OnWindowObjectReady(View* caller, uint64_t /*frame_id*/, bool is_main_frame,
                                             const String& /*url*/) {
        if (!is_main_frame) {
            return;
        }

        {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId_);
            if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) {
                return;
            }
        }

        logger::info("View [{}]: LoadListener: Window object ready.", viewId_);
        PapyrusBridge::InjectBridge(caller, viewId_);

        {
            const auto networkBlockScript = NetworkSandbox::BuildNetworkBlockScript();
            ultralight::String exception;
            caller->EvaluateScript(ultralight::String(networkBlockScript.c_str()), &exception);
            if (!exception.empty()) {
                logger::warn("View [{}]: Network sandbox injection failed: {}", viewId_, exception.utf8().data());
            } else {
                logger::debug("View [{}]: Network sandbox injected.", viewId_);
            }
        }

        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire) ||
            it->second->translationPluginName.empty()) {
            return;
        }

        std::string pluginName = it->second->translationPluginName;
        lock.unlock();

        auto lang = Translations::DetectGameLanguage();
        auto map = Translations::ParseTranslationFile(pluginName, lang);
        auto script = Translations::BuildL10NScript(map);
        if (script.empty()) {
            return;
        }

        ultralight::String ulScript(script.c_str());
        caller->EvaluateScript(ulScript);
        logger::info("View [{}]: Injected L10N for '{}' ({} keys, lang={})", viewId_, pluginName, map.size(), lang);
    }

    void MyLoadListener::OnDOMReady(View* /*caller*/, uint64_t /*frame_id*/, bool is_main_frame,
                                    const String& /*url*/) {
        if (!is_main_frame) {
            return;
        }

        logger::info("View [{}]: LoadListener: DOM ready.", viewId_);
        ultralightThread.submit([id = viewId_] {
            std::function<void(const PrismaViewId&)> callback;
            {
                std::shared_lock lock(viewsMutex);
                auto it = views.find(id);
                if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) {
                    return;
                }
                callback = it->second->domReadyCallback;
            }

            if (callback) {
                callback(id);
            }
        });
    }

    MyViewListener::MyViewListener(Core::PrismaViewId id) : viewId_(std::move(id)) {}

    MyViewListener::~MyViewListener() = default;

    RefPtr<View> MyViewListener::OnCreateChildView(View* /*caller*/, const String& /*opener_url*/,
                                                    const String& target_url, bool /*is_popup*/,
                                                    const IntRect& /*popup_rect*/) {
        logger::warn("[PrismaUI Security] Blocked child view navigation to '{}'", target_url.utf8().data());
        return nullptr;
    }

    void MyViewListener::OnAddConsoleMessage(ultralight::View* /*caller*/,
                                              const ultralight::ConsoleMessage& message) {
        if (message.source() == kMessageSource_Network) {
            logger::warn("[PrismaUI Security] View [{}]: [Network] {}", viewId_, message.message().utf8().data());
        }

        std::function<void(Core::PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback;
        PRISMA_UI_API::ConsoleMessageLevel prismaLevel = PRISMA_UI_API::ConsoleMessageLevel::Log;
        {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId_);
            if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire) ||
                !it->second->consoleMessageCallback) {
                return;
            }

            switch (message.level()) {
                case kMessageLevel_Warning:
                    prismaLevel = PRISMA_UI_API::ConsoleMessageLevel::Warning;
                    break;
                case kMessageLevel_Error:
                    prismaLevel = PRISMA_UI_API::ConsoleMessageLevel::Error;
                    break;
                case kMessageLevel_Debug:
                    prismaLevel = PRISMA_UI_API::ConsoleMessageLevel::Debug;
                    break;
                case kMessageLevel_Info:
                    prismaLevel = PRISMA_UI_API::ConsoleMessageLevel::Info;
                    break;
                default:
                    break;
            }
            callback = it->second->consoleMessageCallback;
        }

        callback(viewId_, prismaLevel, std::string(message.message().utf8().data()));
    }

    RefPtr<View> MyViewListener::OnCreateInspectorView(View* /*caller*/, bool is_local, const String& inspectedURL) {
        logger::info("View [{}]: ViewListener: OnCreateInspectorView called (is_local={}, URL={})", viewId_, is_local,
                     inspectedURL.utf8().data());

        RefPtr<View> inspectorView = nullptr;
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) {
            return nullptr;
        }

        auto viewData = it->second;
        if (!viewData->inspectorView && viewData->ultralightView && renderer) {
            uint32_t width = viewData->inspectorDisplayWidth > 0 ? viewData->inspectorDisplayWidth : 800;
            uint32_t height = viewData->inspectorDisplayHeight > 0 ? viewData->inspectorDisplayHeight : 600;

            ViewConfig config;
            config.is_accelerated = false;
            config.is_transparent = true;

            viewData->inspectorView = renderer->CreateView(width, height, config, nullptr);
            inspectorView = viewData->inspectorView;
            logger::info("View [{}]: Inspector view created with size {}x{}", viewId_, width, height);
        } else if (viewData->inspectorView) {
            inspectorView = viewData->inspectorView;
            logger::info("View [{}]: Returning existing inspector view", viewId_);
        }

        return inspectorView;
    }

    MyUltralightLogger::~MyUltralightLogger() = default;

    void MyUltralightLogger::LogMessage(LogLevel /*log_level*/, const String& /*message*/) {}
}
