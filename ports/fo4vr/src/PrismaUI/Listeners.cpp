#include "PCH.h"

#include "PrismaUI/Listeners.h"

#include "PrismaUI/Communication.h"
#include "PrismaUI/Inspector.h"
#include "PrismaUI/InputHandler.h"
#include "PrismaUI/Translations.h"
#include "PrismaUI/ViewManager.h"
#include "PrismaUI/VirtualFilePolicy.h"
#include "Utils/UrlDiagnostics.h"

namespace PrismaUI::Listeners
{
    namespace
    {
        [[nodiscard]] std::string SanitizedUrl(
            const ultralight::String& url) noexcept
        {
            try {
                const auto& utf8 = url.utf8();
                return UrlDiagnostics::Sanitize(
                    std::string_view(utf8.data(), utf8.length()));
            } catch (...) {
                return "<url-unavailable>";
            }
        }

        template <class Callback>
        void LogLifecycle(
            std::atomic<std::uint32_t>& count,
            Core::PrismaViewId viewId,
            Callback&& callback) noexcept
        {
            try {
                const auto index =
                    count.fetch_add(1, std::memory_order_relaxed);
                if (index < 8) {
                    callback();
                } else if (index == 8) {
                    logger::warn(
                        "View [{}] load lifecycle logging is now suppressed",
                        viewId);
                }
            } catch (...) {
            }
        }

        void MarkNotReady(Core::PrismaViewId viewId) noexcept
        {
            if (const auto view = Core::FindView(viewId)) {
                view->loadingFinished.store(
                    false,
                    std::memory_order_release);
            }
        }

        template <class Callback>
        void PostListenerWork(
            Core::PrismaViewId viewId,
            Callback&& callback) noexcept
        {
            auto operation =
                [viewId,
                 callback = std::forward<Callback>(callback)]() mutable noexcept {
                    try {
                        callback();
                    } catch (...) {
                        MarkNotReady(viewId);
                        logger::error(
                            "View [{}] load transition failed closed",
                            viewId);
                    }
                };
            if (!Core::GetRuntime().worker.TryPost(
                    SingleThreadExecutor::Priority::HIGH,
                    std::move(operation))) {
                MarkNotReady(viewId);
                logger::error(
                    "View [{}] load transition could not be queued",
                    viewId);
            }
        }

        [[nodiscard]] bool IsKnownNetworkPolicy(
            PRISMA_UI_VR_API::NetworkAccessPolicy policy) noexcept
        {
            using enum PRISMA_UI_VR_API::NetworkAccessPolicy;
            return policy == Unrestricted ||
                   policy == LocalOnly ||
                   policy == RemoteNoFile;
        }
    }

    LoadListener::LoadListener(Core::PrismaViewId viewId) noexcept :
        viewId_(viewId)
    {}

    void LoadListener::OnBeginLoading(
        ultralight::View*,
        std::uint64_t,
        bool isMainFrame,
        const ultralight::String& url) noexcept
    {
        if (!isMainFrame) {
            return;
        }
        LogLifecycle(lifecycleLogCount_, viewId_, [&] {
            logger::info(
                "View [{}] began loading {}",
                viewId_,
                SanitizedUrl(url));
        });

        const auto view = Core::FindView(viewId_);
        if (!view) {
            return;
        }
        view->loadingFinished.store(false, std::memory_order_release);
        view->windowObjectReady.store(false, std::memory_order_release);
        view->injectedTranslationRevision.store(
            0,
            std::memory_order_release);
        if (view->focused.load(std::memory_order_acquire) ||
            view->focusRequestPending.load(std::memory_order_acquire)) {
            ViewManager::Unfocus(viewId_);
        }
    }

    void LoadListener::OnFinishLoading(
        ultralight::View*,
        std::uint64_t,
        bool isMainFrame,
        const ultralight::String& url) noexcept
    {
        if (!isMainFrame) {
            return;
        }
        LogLifecycle(lifecycleLogCount_, viewId_, [&] {
            logger::info(
                "View [{}] finished loading {}",
                viewId_,
                SanitizedUrl(url));
        });

        PostListenerWork(viewId_, [viewId = viewId_] {
            const auto view = Core::FindView(viewId);
            if (!view ||
                view->destroying.load(std::memory_order_acquire)) {
                return;
            }
            const auto bound =
                Communication::BindJSCallbacks(viewId);
            view->loadingFinished.store(
                bound,
                std::memory_order_release);
        });
    }

    void LoadListener::OnFailLoading(
        ultralight::View*,
        std::uint64_t,
        bool isMainFrame,
        const ultralight::String& url,
        const ultralight::String&,
        const ultralight::String&,
        int errorCode) noexcept
    {
        if (!isMainFrame) {
            return;
        }
        MarkNotReady(viewId_);
        LogLifecycle(lifecycleLogCount_, viewId_, [&] {
            logger::error(
                "View [{}] failed to load {} (error {})",
                viewId_,
                SanitizedUrl(url),
                errorCode);
        });
    }

    void LoadListener::OnWindowObjectReady(
        ultralight::View* caller,
        std::uint64_t,
        bool isMainFrame,
        const ultralight::String&) noexcept
    {
        if (!isMainFrame || !caller) {
            return;
        }
        const auto view = Core::FindView(viewId_);
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return;
        }
        view->windowObjectReady.store(true, std::memory_order_release);
        (void)Translations::InjectForCurrentWindow(*view, caller);
        InputHandler::RefreshTextInputTracking(viewId_);
    }

    void LoadListener::OnDOMReady(
        ultralight::View*,
        std::uint64_t,
        bool isMainFrame,
        const ultralight::String&) noexcept
    {
        if (!isMainFrame) {
            return;
        }
        PostListenerWork(viewId_, [viewId = viewId_] {
            const auto view = Core::FindView(viewId);
            if (!view ||
                view->destroying.load(std::memory_order_acquire)) {
                return;
            }

            const auto bound =
                Communication::BindJSCallbacks(viewId);
            view->loadingFinished.store(
                bound,
                std::memory_order_release);
            if (bound) {
                InputHandler::RefreshTextInputTracking(viewId);
            }
            if (bound && view->domReadyCallback) {
                try {
                    view->domReadyCallback(viewId);
                } catch (...) {
                    logger::error(
                        "View [{}] consumer DOM-ready callback failed",
                        viewId);
                }
            }
        });
    }

    ViewListener::ViewListener(Core::PrismaViewId viewId) noexcept :
        viewId_(viewId)
    {}

    void ViewListener::OnAddConsoleMessage(
        ultralight::View*,
        const ultralight::ConsoleMessage& message) noexcept
    {
        try {
            const auto view = Core::FindView(viewId_);
            if (!view) {
                return;
            }
            decltype(view->consoleMessageCallback) callback;
            {
                std::lock_guard lock(view->callbackMutex);
                callback = view->consoleMessageCallback;
            }
            if (!callback) {
                return;
            }

            auto level = PRISMA_UI_API::ConsoleMessageLevel::Log;
            switch (message.level()) {
            case ultralight::kMessageLevel_Warning:
                level = PRISMA_UI_API::ConsoleMessageLevel::Warning;
                break;
            case ultralight::kMessageLevel_Error:
                level = PRISMA_UI_API::ConsoleMessageLevel::Error;
                break;
            case ultralight::kMessageLevel_Debug:
                level = PRISMA_UI_API::ConsoleMessageLevel::Debug;
                break;
            case ultralight::kMessageLevel_Info:
                level = PRISMA_UI_API::ConsoleMessageLevel::Info;
                break;
            default:
                break;
            }

            const auto& utf8 = message.message().utf8();
            std::string text(utf8.data(), utf8.length());
            callback(viewId_, level, text);
        } catch (...) {
            logger::error(
                "View [{}] console callback failed",
                viewId_);
        }
    }

    ultralight::RefPtr<ultralight::View>
        ViewListener::OnCreateChildView(
            ultralight::View*,
            const ultralight::String&,
            const ultralight::String& targetUrl,
            bool,
            const ultralight::IntRect&) noexcept
    {
        const auto index =
            blockedChildViewCount_.fetch_add(
                1,
                std::memory_order_relaxed);
        if (index < 4) {
            logger::warn(
                "View [{}] blocked child-window request to {}",
                viewId_,
                SanitizedUrl(targetUrl));
        } else if (index == 4) {
            logger::warn(
                "View [{}] further child-window logs are suppressed",
                viewId_);
        }
        return nullptr;
    }

    ultralight::RefPtr<ultralight::View>
        ViewListener::OnCreateInspectorView(
            ultralight::View*,
            bool,
            const ultralight::String&) noexcept
    {
        return Inspector::HandleCreateRequest(viewId_);
    }

    NetworkListener::NetworkListener(
        Core::PrismaViewId viewId,
        const std::atomic<
            PRISMA_UI_VR_API::NetworkAccessPolicy>* policy) noexcept :
        viewId_(viewId),
        policy_(policy)
    {}

    bool NetworkListener::OnNetworkRequest(
        ultralight::View*,
        ultralight::NetworkRequest& request) noexcept
    {
        try {
            const auto policy = policy_ ?
                policy_->load(std::memory_order_acquire) :
                PRISMA_UI_VR_API::NetworkAccessPolicy::LocalOnly;
            if (!IsKnownNetworkPolicy(policy)) {
                return false;
            }
            if (policy ==
                PRISMA_UI_VR_API::NetworkAccessPolicy::Unrestricted) {
                return true;
            }

            const auto protocolValue = request.urlProtocol();
            const auto& protocolUtf8 = protocolValue.utf8();
            std::string protocol(
                protocolUtf8.data(),
                protocolUtf8.length());
            std::transform(
                protocol.begin(),
                protocol.end(),
                protocol.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });

            const auto hostValue = request.urlHost();
            const auto& hostUtf8 = hostValue.utf8();
            std::string host(hostUtf8.data(), hostUtf8.length());
            std::transform(
                host.begin(),
                host.end(),
                host.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });

            const auto urlValue = request.url();
            const auto& urlUtf8 = urlValue.utf8();
            const std::string_view url(
                urlUtf8.data(),
                urlUtf8.length());
            const auto localFile =
                protocol == "file" &&
                VirtualFilePolicy::IsSafeFileUrl(url, host);
            const auto localProtocol =
                localFile ||
                protocol == "data" ||
                protocol == "blob" ||
                protocol == "about" ||
                (protocol.empty() && host.empty());
            const auto allowed =
                policy ==
                        PRISMA_UI_VR_API::NetworkAccessPolicy::RemoteNoFile ?
                    protocol != "file" :
                    localProtocol;
            if (allowed) {
                return true;
            }

            const auto index =
                blockedRequestCount_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            if (index < 4) {
                logger::warn(
                    "View [{}] blocked request protocol='{}' host='{}'",
                    viewId_,
                    protocol,
                    host);
            } else if (index == 4) {
                logger::warn(
                    "View [{}] further blocked-request logs are suppressed",
                    viewId_);
            }
            return false;
        } catch (...) {
            logger::error(
                "View [{}] network policy failed closed",
                viewId_);
            return false;
        }
    }

    void UltralightLogger::LogMessage(
        ultralight::LogLevel level,
        const ultralight::String&)
    {
        if (level != ultralight::LogLevel::Error) {
            return;
        }
        const auto index =
            errorCount_.fetch_add(1, std::memory_order_relaxed);
        if (index < 8) {
            logger::error(
                "Ultralight reported an internal error; message omitted");
        } else if (index == 8) {
            logger::error(
                "Further Ultralight internal-error logs are suppressed");
        }
    }
}
