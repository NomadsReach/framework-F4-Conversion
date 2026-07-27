#include "PCH.h"

#include "PrismaUI/ViewManager.h"

#include "Menus/FocusMenu/FocusMenu.h"
#include "PrismaUI/InputHandler.h"
#include "PrismaUI/Inspector.h"
#include "PrismaUI/Listeners.h"
#include "PrismaUI/SpatialPointer.h"
#include "PrismaUI/SpatialPresentation.h"
#include "PrismaUI/Translations.h"
#include "PrismaUI/VirtualFilePolicy.h"
#include "Utils/UrlDiagnostics.h"

namespace PrismaUI::ViewManager
{
    namespace
    {
        enum class ViewSource : std::uint8_t
        {
            Invalid,
            Local,
            Remote
        };

        [[nodiscard]] ViewSource Classify(
            std::string_view value) noexcept
        {
            if (value.empty() ||
                value.size() >
                    VirtualFilePolicy::kMaximumPathBytes ||
                std::any_of(
                    value.begin(),
                    value.end(),
                    [](unsigned char character) {
                        return character < 0x20u ||
                               character == 0x7Fu;
                    })) {
                return ViewSource::Invalid;
            }

            const auto http =
                UrlDiagnostics::StartsWithInsensitive(
                    value,
                    "http://");
            const auto https =
                UrlDiagnostics::StartsWithInsensitive(
                    value,
                    "https://");
            if (http || https) {
                const auto authorityStart =
                    https ? std::size_t{8} : std::size_t{7};
                const auto authorityEnd =
                    value.find_first_of("/?#", authorityStart);
                const auto authority = value.substr(
                    authorityStart,
                    authorityEnd == std::string_view::npos ?
                        std::string_view::npos :
                        authorityEnd - authorityStart);
                if (authority.empty() ||
                    authority.find('@') != std::string_view::npos ||
                    value.find('\\') != std::string_view::npos) {
                    return ViewSource::Invalid;
                }
                return ViewSource::Remote;
            }
            return VirtualFilePolicy::IsSafeRelativePath(value) ?
                ViewSource::Local :
                ViewSource::Invalid;
        }

        [[nodiscard]] bool IsKnownPolicy(
            PRISMA_UI_VR_API::NetworkAccessPolicy policy) noexcept
        {
            using enum PRISMA_UI_VR_API::NetworkAccessPolicy;
            return policy == Unrestricted ||
                   policy == LocalOnly ||
                   policy == RemoteNoFile;
        }

        [[nodiscard]] bool SourceMatchesPolicy(
            ViewSource source,
            PRISMA_UI_VR_API::NetworkAccessPolicy policy) noexcept
        {
            using enum PRISMA_UI_VR_API::NetworkAccessPolicy;
            if (source == ViewSource::Local) {
                return policy == LocalOnly ||
                       policy == Unrestricted;
            }
            if (source == ViewSource::Remote) {
                return policy == RemoteNoFile ||
                       policy == Unrestricted;
            }
            return false;
        }

        [[nodiscard]] bool QueueGameTask(
            std::string_view name,
            std::function<void()> operation) noexcept
        {
            const auto tasks = F4SE::GetTaskInterface();
            if (!tasks || !operation) {
                logger::error(
                    "{}: F4SE game task interface is unavailable",
                    name);
                return false;
            }
            try {
                tasks->AddTask(
                    [name = std::string(name),
                     operation = std::move(operation)]() mutable noexcept {
                        try {
                            operation();
                        } catch (...) {
                            try {
                                logger::error(
                                    "{}: game task failed",
                                    name);
                            } catch (...) {
                            }
                        }
                    });
                return true;
            } catch (...) {
                logger::error(
                    "{}: could not queue a game task",
                    name);
                return false;
            }
        }

        void CancelFocusRequest(
            const std::shared_ptr<Core::PrismaView>& view) noexcept
        {
            if (!view) {
                return;
            }
            std::lock_guard lock(view->focusRequestMutex);
            view->focusRequestGeneration.fetch_add(
                1,
                std::memory_order_acq_rel);
            view->focusRequestPending.store(
                false,
                std::memory_order_release);
        }

        // Game-thread only.
        void ReleaseGameFocus(
            const std::shared_ptr<Core::PrismaView>& view,
            bool closeMenu,
            bool releaseControlMap) noexcept
        {
            if (!view) {
                return;
            }

            view->focused.store(false, std::memory_order_release);
            const auto usedMenu = view->usesFocusMenu.exchange(
                false,
                std::memory_order_acq_rel);
            if (view->paused.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                const auto ui = RE::UI::GetSingleton();
                if (ui && ui->freezeFramePause > 0) {
                    --ui->freezeFramePause;
                }
            }

            InputHandler::DisableInputCapture(view->id);
            if (closeMenu &&
                usedMenu &&
                !InputHandler::IsAnyInputCaptureActive()) {
                FocusMenu::Close();
            }
            if (releaseControlMap &&
                !InputHandler::IsAnyInputCaptureActive()) {
                if (const auto controls =
                        RE::ControlMap::GetSingleton()) {
                    controls->SetIgnoreKeyboardMouse(false);
                }
            }
        }

        // Game-thread only.
        void ApplyGameFocus(
            const std::shared_ptr<Core::PrismaView>& view,
            bool pauseGame,
            bool disableFocusMenu,
            std::uint64_t generation) noexcept
        {
            if (!view) {
                return;
            }

            struct PendingReset
            {
                std::shared_ptr<Core::PrismaView> view;
                std::uint64_t generation;
                ~PendingReset()
                {
                    if (view &&
                        view->focusRequestGeneration.load(
                            std::memory_order_acquire) ==
                            generation) {
                        view->focusRequestPending.store(
                            false,
                            std::memory_order_release);
                    }
                }
            } reset{view, generation};

            if (Core::IsShuttingDown() ||
                view->destroying.load(std::memory_order_acquire) ||
                view->hidden.load(std::memory_order_acquire) ||
                !view->loadingFinished.load(
                    std::memory_order_acquire) ||
                !view->focusRequestPending.load(
                    std::memory_order_acquire) ||
                view->focusRequestGeneration.load(
                    std::memory_order_acquire) != generation ||
                SpatialPresentation::
                    HasRenderOnlyWorldPresentation(view)) {
                return;
            }

            const auto previousId =
                InputHandler::GetFocusedViewId();
            const auto previous =
                previousId != 0 &&
                        previousId != view->id ?
                    Core::FindView(previousId) :
                    nullptr;

            if (!InputHandler::EnableInputCapture(view->id)) {
                logger::warn(
                    "View [{}] could not acquire PrismaUI input",
                    view->id);
                return;
            }
            if (previous) {
                ReleaseGameFocus(
                    previous,
                    false,
                    false);
            }
            view->focused.store(true, std::memory_order_release);

            const auto useMenu = !disableFocusMenu;
            view->usesFocusMenu.store(
                useMenu,
                std::memory_order_release);
            if (useMenu) {
                FocusMenu::Open();
            } else {
                FocusMenu::Close();
            }

            if (const auto controls =
                    RE::ControlMap::GetSingleton()) {
                controls->SetIgnoreKeyboardMouse(true);
            }
            if (pauseGame &&
                !view->paused.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                const auto ui = RE::UI::GetSingleton();
                if (ui) {
                    ++ui->freezeFramePause;
                } else {
                    view->paused.store(
                        false,
                        std::memory_order_release);
                }
            }
        }

        [[nodiscard]] bool QueueFocus(
            const std::shared_ptr<Core::PrismaView>& view,
            bool pauseGame,
            bool disableFocusMenu) noexcept
        {
            if (!view) {
                return false;
            }

            std::uint64_t generation = 0;
            {
                std::lock_guard lock(view->focusRequestMutex);
                if (view->focusRequestPending.load(
                        std::memory_order_relaxed)) {
                    return true;
                }
                generation =
                    view->focusRequestGeneration.fetch_add(
                        1,
                        std::memory_order_acq_rel) + 1;
                view->focusRequestPending.store(
                    true,
                    std::memory_order_release);
            }

            if (QueueGameTask(
                    "PrismaUI focus",
                    [view,
                     pauseGame,
                     disableFocusMenu,
                     generation] {
                        ApplyGameFocus(
                            view,
                            pauseGame,
                            disableFocusMenu,
                            generation);
                    })) {
                return true;
            }

            CancelFocusRequest(view);
            return false;
        }
    }

    Core::PrismaViewId Create(
        const std::string& htmlPath,
        std::function<void(Core::PrismaViewId)> onDomReady,
        PRISMA_UI_VR_API::NetworkAccessPolicy networkPolicy)
    {
        const auto source = Classify(htmlPath);
        if (!IsKnownPolicy(networkPolicy) ||
            !SourceMatchesPolicy(source, networkPolicy)) {
            logger::error(
                "PrismaUI rejected an unsafe view source or incompatible network policy");
            return 0;
        }
        if (Core::IsShuttingDown() ||
            !Core::StartUltralightWorker()) {
            return 0;
        }

        auto view = std::make_shared<Core::PrismaView>(0);
        view->originalHtmlPath = htmlPath;
        view->htmlPathToLoad =
            source == ViewSource::Remote ?
                htmlPath :
                "file:///views/" + htmlPath;
        view->originalUrl = view->htmlPathToLoad;
        view->networkAccessPolicy.store(
            networkPolicy,
            std::memory_order_relaxed);
        view->domReadyCallback = std::move(onDomReady);

        auto& runtime = Core::GetRuntime();
        {
            std::unique_lock lock(runtime.viewsMutex);
            if (runtime.shuttingDown.load(
                    std::memory_order_acquire) ||
                runtime.views.size() >=
                    Core::kMaximumFrameworkViews) {
                return 0;
            }

            constexpr std::size_t maximumAttempts =
                Core::kMaximumFrameworkViews * 2;
            for (std::size_t attempt = 0;
                 attempt < maximumAttempts;
                 ++attempt) {
                const auto candidate =
                    runtime.idGenerator.generate();
                if (candidate != 0 &&
                    !runtime.views.contains(candidate)) {
                    view->id = candidate;
                    break;
                }
            }
            if (view->id == 0) {
                return 0;
            }

            auto maximumOrder = -1;
            for (const auto& [id, existing] : runtime.views) {
                (void)id;
                if (existing) {
                    maximumOrder = (std::max)(
                        maximumOrder,
                        existing->order.load(
                            std::memory_order_relaxed));
                }
            }
            view->order.store(
                maximumOrder == (std::numeric_limits<int>::max)() ?
                    maximumOrder :
                    maximumOrder + 1,
                std::memory_order_relaxed);
            runtime.views.emplace(view->id, view);
        }

        logger::info(
            "View [{}] accepted for {}",
            view->id,
            UrlDiagnostics::Sanitize(view->htmlPathToLoad));
        return view->id;
    }

    void Show(Core::PrismaViewId viewId) noexcept
    {
        if (const auto view = Core::FindView(viewId)) {
            view->hidden.store(false, std::memory_order_release);
        }
    }

    void Hide(Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view) {
            return;
        }
        view->hidden.store(true, std::memory_order_release);
        view->deferredFocusPending.store(
            false,
            std::memory_order_release);
        CancelFocusRequest(view);
        (void)SpatialPointer::CancelForView(view, true);
        Unfocus(viewId);
    }

    bool IsHidden(Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        return !view ||
               view->hidden.load(std::memory_order_acquire);
    }

    bool Focus(
        Core::PrismaViewId viewId,
        bool pauseGame,
        bool disableFocusMenu) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view ||
            view->hidden.load(std::memory_order_acquire) ||
            !view->loadingFinished.load(
                std::memory_order_acquire)) {
            return false;
        }

        if (SpatialPresentation::
                HasRenderOnlyWorldPresentation(view)) {
            if (!SpatialPresentation::
                    IsTransitioningToHeadLocked(view)) {
                return false;
            }
            view->deferredFocusPauseGame.store(
                pauseGame,
                std::memory_order_relaxed);
            view->deferredFocusDisableFocusMenu.store(
                disableFocusMenu,
                std::memory_order_relaxed);
            view->deferredFocusPending.store(
                true,
                std::memory_order_release);
            return true;
        }
        return QueueFocus(
            view,
            pauseGame,
            disableFocusMenu);
    }

    void Unfocus(Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view) {
            return;
        }
        view->deferredFocusPending.store(
            false,
            std::memory_order_release);
        CancelFocusRequest(view);
        (void)QueueGameTask(
            "PrismaUI unfocus",
            [view] {
                ReleaseGameFocus(view, true, true);
            });
    }

    bool HasFocus(Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        return view &&
               view->focused.load(std::memory_order_acquire);
    }

    bool ViewHasInputFocus(Core::PrismaViewId viewId) noexcept
    {
        return HasFocus(viewId) &&
               InputHandler::IsInputCaptureActiveForView(viewId);
    }

    void Destroy(Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view ||
            view->destroying.exchange(
                true,
                std::memory_order_acq_rel)) {
            return;
        }
        const auto wasHidden = view->hidden.exchange(
            true,
            std::memory_order_acq_rel);
        view->deferredFocusPending.store(
            false,
            std::memory_order_release);
        CancelFocusRequest(view);
        (void)SpatialPointer::CancelForView(view, true);

        if (!QueueGameTask(
                "PrismaUI destroy",
                [view, wasHidden] {
                    auto& runtime = Core::GetRuntime();
                    const auto accepted =
                        runtime.worker.TryPost(
                            SingleThreadExecutor::Priority::HIGH,
                            [view] {
                                try {
                                    if (view->ultralightView) {
                                        view->ultralightView->
                                            set_load_listener(nullptr);
                                        view->ultralightView->
                                            set_view_listener(nullptr);
                                        view->ultralightView->
                                            set_network_listener(nullptr);
                                        view->ultralightView->Unfocus();
                                    }
                                    if (view->inspectorView) {
                                        view->inspectorView->Unfocus();
                                    }
                                } catch (...) {
                                    logger::error(
                                        "View [{}] listener detachment failed",
                                        view->id);
                                }

                                view->inspectorView = nullptr;
                                view->ultralightView = nullptr;
                                view->loadListener.reset();
                                view->viewListener.reset();
                                view->networkListener.reset();
                                Core::ReleaseViewPixelReservations(view);
                                {
                                    std::lock_guard lock(
                                        view->renderTargetMutex);
                                    view->renderTarget = {};
                                    view->inspectorRenderTarget = {};
                                    view->pendingRenderTarget = {};
                                    view->pendingInspectorRenderTarget = {};
                                }
                                {
                                    std::lock_guard lock(
                                        view->translationMutex);
                                    view->translationScript.reset();
                                    view->translationPluginName.clear();
                                }
                                view->loadingFinished.store(
                                    false,
                                    std::memory_order_release);
                            });
                    if (!accepted) {
                        view->hidden.store(
                            wasHidden,
                            std::memory_order_release);
                        view->destroying.store(
                            false,
                            std::memory_order_release);
                        logger::error(
                            "View [{}] destruction was rejected by the Ultralight worker",
                            view->id);
                        return;
                    }

                    ReleaseGameFocus(view, true, true);
                    {
                        std::unique_lock lock(runtime.viewsMutex);
                        const auto iterator =
                            runtime.views.find(view->id);
                        if (iterator != runtime.views.end() &&
                            iterator->second == view) {
                            runtime.views.erase(iterator);
                        }
                    }
                    {
                        std::lock_guard lock(
                            runtime.jsCallbacksMutex);
                        for (auto iterator =
                                 runtime.jsCallbacks.begin();
                             iterator !=
                                 runtime.jsCallbacks.end();) {
                            if (iterator->first.first ==
                                view->id) {
                                iterator =
                                    runtime.jsCallbacks.erase(
                                        iterator);
                            } else {
                                ++iterator;
                            }
                        }
                    }
                })) {
            view->hidden.store(
                wasHidden,
                std::memory_order_release);
            view->destroying.store(
                false,
                std::memory_order_release);
        }
    }

    bool IsValid(Core::PrismaViewId viewId) noexcept
    {
        return Core::FindView(viewId) != nullptr;
    }

    void SetScrollingPixelSize(
        Core::PrismaViewId viewId,
        int pixelSize) noexcept
    {
        if (const auto view = Core::FindView(viewId)) {
            view->scrollingPixelSize.store(
                std::clamp(pixelSize, 1, 4096),
                std::memory_order_release);
        }
    }

    int GetScrollingPixelSize(
        Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        return view ?
            view->scrollingPixelSize.load(
                std::memory_order_acquire) :
            28;
    }

    void SetOrder(
        Core::PrismaViewId viewId,
        int order) noexcept
    {
        if (const auto view = Core::FindView(viewId)) {
            view->order.store(order, std::memory_order_release);
        }
    }

    int GetOrder(Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        return view ?
            view->order.load(std::memory_order_acquire) :
            -1;
    }

    void CreateInspectorView(
        Core::PrismaViewId viewId) noexcept
    {
        Inspector::CreateInspectorView(viewId);
    }

    void SetInspectorVisibility(
        Core::PrismaViewId viewId,
        bool visible) noexcept
    {
        Inspector::SetInspectorVisibility(viewId, visible);
    }

    bool IsInspectorVisible(
        Core::PrismaViewId viewId) noexcept
    {
        return Inspector::IsInspectorVisible(viewId);
    }

    void SetInspectorBounds(
        Core::PrismaViewId viewId,
        float topLeftX,
        float topLeftY,
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        Inspector::SetInspectorBounds(
            viewId,
            topLeftX,
            topLeftY,
            width,
            height);
    }

    bool HasAnyActiveFocus() noexcept
    {
        return InputHandler::IsAnyInputCaptureActive();
    }

    void ReleaseAllFocus() noexcept
    {
        std::vector<std::shared_ptr<Core::PrismaView>> snapshot;
        auto& runtime = Core::GetRuntime();
        {
            std::shared_lock lock(runtime.viewsMutex);
            snapshot.reserve(runtime.views.size());
            for (const auto& [id, view] : runtime.views) {
                (void)id;
                if (view) {
                    snapshot.push_back(view);
                }
            }
        }
        for (const auto& view : snapshot) {
            view->deferredFocusPending.store(
                false,
                std::memory_order_release);
            CancelFocusRequest(view);
        }

        if (!QueueGameTask(
                "PrismaUI release focus",
                [snapshot = std::move(snapshot)] {
                    for (const auto& view : snapshot) {
                        ReleaseGameFocus(
                            view,
                            false,
                            false);
                    }
                    FocusMenu::Close();
                    if (const auto controls =
                            RE::ControlMap::GetSingleton()) {
                        controls->SetIgnoreKeyboardMouse(false);
                    }
                })) {
            const auto focused =
                InputHandler::GetFocusedViewId();
            if (focused != 0) {
                InputHandler::DisableInputCapture(focused);
            }
        }
    }

    void CancelDeferredFocus(
        const std::shared_ptr<Core::PrismaView>& view) noexcept
    {
        if (view) {
            view->deferredFocusPending.store(
                false,
                std::memory_order_release);
        }
    }

    void ApplyDeferredFocusIfReady(
        const std::shared_ptr<Core::PrismaView>& view) noexcept
    {
        if (!view ||
            SpatialPresentation::
                HasRenderOnlyWorldPresentation(view) ||
            !view->deferredFocusPending.exchange(
                false,
                std::memory_order_acq_rel)) {
            return;
        }
        (void)QueueFocus(
            view,
            view->deferredFocusPauseGame.load(
                std::memory_order_relaxed),
            view->deferredFocusDisableFocusMenu.load(
                std::memory_order_relaxed));
    }

    void RegisterConsoleCallback(
        Core::PrismaViewId viewId,
        std::function<void(
            Core::PrismaViewId,
            PRISMA_UI_API::ConsoleMessageLevel,
            const std::string&)> callback) noexcept
    {
        if (const auto view = Core::FindView(viewId)) {
            std::lock_guard lock(view->callbackMutex);
            view->consoleMessageCallback = std::move(callback);
        }
    }

    void RegisterTranslations(
        Core::PrismaViewId viewId,
        const std::string& pluginName) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view || pluginName.empty()) {
            return;
        }

        try {
            auto script =
                Translations::LoadTranslationScript(pluginName);
            if (!script) {
                return;
            }
            {
                std::lock_guard lock(view->translationMutex);
                view->translationPluginName = pluginName;
                view->translationScript = std::move(script);
                view->translationRevision.fetch_add(
                    1,
                    std::memory_order_acq_rel);
            }
            view->injectedTranslationRevision.store(
                0,
                std::memory_order_release);
            if (!Core::GetRuntime().worker.TryPost(
                    SingleThreadExecutor::Priority::LOW,
                    [view] {
                        if (view->ultralightView) {
                            (void)Translations::
                                InjectForCurrentWindow(
                                    *view,
                                    view->ultralightView.get());
                        }
                    })) {
                logger::warn(
                    "View [{}] translation injection was rejected",
                    viewId);
            }
        } catch (...) {
            logger::error(
                "View [{}] translation registration failed",
                viewId);
        }
    }

    void EnumerateViews(
        const std::function<void(
            Core::PrismaViewId,
            const std::string&)>& callback) noexcept
    {
        if (!callback) {
            return;
        }
        try {
            std::vector<
                std::pair<Core::PrismaViewId, std::string>>
                snapshot;
            auto& runtime = Core::GetRuntime();
            {
                std::shared_lock lock(runtime.viewsMutex);
                snapshot.reserve(runtime.views.size());
                for (const auto& [id, view] : runtime.views) {
                    if (view &&
                        !view->destroying.load(
                            std::memory_order_acquire)) {
                        snapshot.emplace_back(
                            id,
                            view->originalHtmlPath);
                    }
                }
            }
            for (const auto& [id, path] : snapshot) {
                callback(id, path);
            }
        } catch (...) {
            logger::error(
                "PrismaUI view enumeration failed");
        }
    }

    bool SetNetworkAccessPolicy(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::NetworkAccessPolicy policy) noexcept
    {
        if (!IsKnownPolicy(policy)) {
            return false;
        }
        const auto view = Core::FindView(viewId);
        if (!view) {
            return false;
        }
        view->networkAccessPolicy.store(
            policy,
            std::memory_order_release);
        return true;
    }

    bool GetNetworkAccessPolicy(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::NetworkAccessPolicy& outPolicy) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view) {
            return false;
        }
        outPolicy = view->networkAccessPolicy.load(
            std::memory_order_acquire);
        return true;
    }
}
