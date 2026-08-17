#include "ViewManager.h"

#include "Core.h"
#include "InputHandler.h"
#include "Inspector.h"
#include "Listeners.h"
#include "ViewOperationQueue.h"

namespace PrismaUI::ViewManager {
    using namespace Core;

    static void QueueNativeInputStateRefresh() {
        F4SE::GetTaskInterface()->AddTask([]() {
            auto controlMap = RE::ControlMap::GetSingleton();
            if (controlMap) {
                controlMap->SetIgnoreKeyboardMouse(PrismaUI::InputHandler::IsAnyInputCaptureActive());
            }
        });
    }

    static void QueueNativeFocusState(const std::shared_ptr<PrismaView>& viewData, bool pauseGame) {
        if (!viewData) {
            QueueNativeInputStateRefresh();
            return;
        }

        if (pauseGame) {
            viewData->isPaused.store(true, std::memory_order_release);
        }

        F4SE::GetTaskInterface()->AddTask([viewData, pauseGame]() {
            if (pauseGame && viewData->isPaused.load(std::memory_order_acquire)) {
                bool expected = false;
                if (viewData->nativePauseApplied.compare_exchange_strong(expected, true,
                                                                         std::memory_order_acq_rel)) {
                    auto ui = RE::UI::GetSingleton();
                    if (ui) {
                        ui->freezeFramePause++;
                        logger::debug("Game pause applied for View [{}]", viewData->id);
                    } else {
                        viewData->nativePauseApplied.store(false, std::memory_order_release);
                    }
                }
            }

            auto controlMap = RE::ControlMap::GetSingleton();
            if (controlMap) {
                controlMap->SetIgnoreKeyboardMouse(PrismaUI::InputHandler::IsAnyInputCaptureActive());
            }
        });
    }

    static void QueueNativeUnfocusState(const std::shared_ptr<PrismaView>& viewData) {
        if (viewData) {
            viewData->isPaused.store(false, std::memory_order_release);
        }

        F4SE::GetTaskInterface()->AddTask([viewData]() {
            if (viewData && !viewData->isPaused.load(std::memory_order_acquire) &&
                viewData->nativePauseApplied.exchange(false, std::memory_order_acq_rel)) {
                auto ui = RE::UI::GetSingleton();
                if (ui && ui->freezeFramePause > 0) {
                    ui->freezeFramePause--;
                    logger::debug("Game pause released for View [{}]", viewData->id);
                }
            }

            auto controlMap = RE::ControlMap::GetSingleton();
            if (controlMap) {
                controlMap->SetIgnoreKeyboardMouse(PrismaUI::InputHandler::IsAnyInputCaptureActive());
            }
        });
    }

    // closeFocusMenu: false when switching focus between views that share the same modal capture menu.
    // Native Fallout state is intentionally queued to the game/UI task interfaces instead of being
    // mutated from the Ultralight worker thread.
    static void PerformUnfocusOperations(const Core::PrismaViewId& viewId, const std::shared_ptr<PrismaView>& viewData,
                                         bool closeFocusMenu = true) {
        PrismaUI::InputHandler::DisableInputCapture(viewId);
        PrismaUI::InputHandler::ClearImeState(viewId);

        if (viewData && viewData->ultralightView) {
            viewData->ultralightView->Unfocus();
        }

        if (closeFocusMenu) {
            FocusMenu::Close();
        }

        if (viewData) {
            QueueNativeUnfocusState(viewData);
        } else {
            QueueNativeInputStateRefresh();
        }
    }

    Core::PrismaViewId Create(const std::string& htmlPath, std::function<void(Core::PrismaViewId)> onDomReadyCallback) {
        bool expected_init = false;
        if (coreInitialized.compare_exchange_strong(expected_init, true)) {
            Core::InitializeCoreSystem();
            if (!renderer) {
                coreInitialized = false;
                logger::critical("Core initialization failed: Renderer not created.");
                throw std::runtime_error("PrismaUI Core Renderer initialization failed.");
            }
        } else if (!renderer) {
            logger::critical("Cannot create HTML view: Core Renderer is null despite initialization flag.");
            throw std::runtime_error("PrismaUI Core Renderer is unexpectedly null.");
        }

        Core::PrismaViewId newViewId = generator.generate();

        // Security: Reject external URLs to prevent loading remote content with F4SE access
        if (htmlPath.substr(0, 7) == "http://" || htmlPath.substr(0, 8) == "https://") {
            logger::error("[PrismaUI Security] CreateView blocked: external URL '{}' is not permitted. "
                          "Only local file paths relative to Data/PrismaUI_F4/views/ are allowed.",
                          htmlPath);
            return 0;
        }
        std::string fileUrl = "file:///views/" + htmlPath;

        auto viewData = std::make_shared<Core::PrismaView>();
        viewData->id = newViewId;
        viewData->ultralightView = nullptr;
        viewData->htmlPathToLoad = fileUrl;
        viewData->originalUrl = fileUrl;  // Store for recovery after exceptions
        viewData->isHidden = false;
        viewData->domReadyCallback = onDomReadyCallback;

        {
            std::unique_lock lock(viewsMutex);
            int maxOrder = -1;
            if (views.empty()) {
                viewData->order = 0;
            } else {
                for (const auto& pair : views) {
                    if (pair.second->order > maxOrder) {
                        maxOrder = pair.second->order;
                    }
                }
                viewData->order = maxOrder + 1;
            }
            views[newViewId] = viewData;
        }

        logger::info(
            "View [{}] creation requested for path: {} with order <{}>. Actual view will be created by UI thread.",
            newViewId, fileUrl, viewData->order);

        return newViewId;
    }

    void Show(const Core::PrismaViewId& viewId) {
        if (!ViewManager::IsValid(viewId)) {
            logger::warn("Show: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            std::shared_ptr<PrismaView> viewData = nullptr;
            {
                std::shared_lock lock(viewsMutex);
                auto it = views.find(viewId);
                if (it != views.end()) {
                    viewData = it->second;
                }
            }

            if (viewData && !viewData->isDestroying.load(std::memory_order_acquire)) {
                if (!viewData->isHidden.load()) {
                    logger::debug("Show: View [{}] is already visible.", viewId);
                    return;
                }
                viewData->isHidden = false;
                logger::debug("View [{}] marked as Visible.", viewId);
            }
        });
    }

    void Hide(const Core::PrismaViewId& viewId) {
        if (!ViewManager::IsValid(viewId)) {
            logger::warn("Hide: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            std::shared_ptr<PrismaView> viewData = nullptr;
            {
                std::shared_lock lock(viewsMutex);
                auto it = views.find(viewId);
                if (it != views.end()) {
                    viewData = it->second;
                }
            }

            if (viewData && !viewData->isDestroying.load(std::memory_order_acquire)) {
                if (viewData->isHidden.load()) {
                    logger::debug("Hide: View [{}] is already hidden.", viewId);
                    return;
                }

                const bool hasUltralightFocus = viewData->ultralightView && viewData->ultralightView->HasFocus();
                const bool ownsInputCapture = PrismaUI::InputHandler::IsInputCaptureActiveForView(viewId);
                const bool ownsPause = viewData->isPaused.load(std::memory_order_acquire) ||
                                       viewData->nativePauseApplied.load(std::memory_order_acquire);
                if (hasUltralightFocus || ownsInputCapture || ownsPause) {
                    PerformUnfocusOperations(viewId, viewData);
                }

                viewData->isHidden = true;
                logger::debug("View [{}] marked as Hidden.", viewId);
            }
        });
    }

    bool IsHidden(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end()) {
            return it->second->isHidden.load() || it->second->isDestroying.load(std::memory_order_acquire);
        }
        logger::warn("IsHidden: View ID [{}] not found.", viewId);
        return true;
    }

    bool IsValid(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        return it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire);
    }

    bool Focus(const Core::PrismaViewId& viewId, bool pauseGame, bool disableFocusMenu) {
        if (!ViewManager::IsValid(viewId)) {
            logger::warn("Focus: View ID [{}] not found.", viewId);
            return false;
        }

        if (!ViewOperationQueue::EnqueueOperation(viewId, [viewId, pauseGame, disableFocusMenu]() {
                std::shared_ptr<PrismaView> viewData = nullptr;
                {
                    std::shared_lock lock(viewsMutex);
                    auto it = views.find(viewId);
                    if (it != views.end()) {
                        viewData = it->second;
                    }
                }

                if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) ||
                    !viewData->ultralightView) {
                    logger::warn("Focus: View [{}] or its Ultralight View is not ready.", viewId);
                    return;
                }

                if (viewData->isHidden.load()) {
                    logger::warn("Focus: View [{}] is hidden, cannot focus.", viewId);
                    return;
                }

                if (viewData->ultralightView->HasFocus()) {
                    PrismaUI::InputHandler::EnableInputCapture(viewId);
                    if (disableFocusMenu) {
                        FocusMenu::Close();
                    } else {
                        FocusMenu::Open();
                    }
                    QueueNativeFocusState(viewData, pauseGame);
                    logger::debug("Focus: View [{}] already had Ultralight focus; native capture state refreshed.",
                                  viewId);
                    return;
                }

                // Unfocus other views immediately while already on the Ultralight worker. Deferring
                // these into separate view queues left a window where two views could report focus and
                // where the old view could be destroyed before native input state was restored.
                std::vector<std::pair<Core::PrismaViewId, std::shared_ptr<PrismaView>>> viewsToUnfocus;
                {
                    std::shared_lock lock(viewsMutex);
                    for (const auto& pair : views) {
                        if (pair.first != viewId && pair.second && !pair.second->isDestroying.load() &&
                            pair.second->ultralightView && pair.second->ultralightView->HasFocus()) {
                            viewsToUnfocus.emplace_back(pair.first, pair.second);
                        }
                    }
                }

                for (const auto& [idToUnfocus, oldView] : viewsToUnfocus) {
                    // If the incoming view has disabled FocusMenu, close the old modal menu as part
                    // of the hand-off. Otherwise keep it open across the switch to avoid churn.
                    PerformUnfocusOperations(idToUnfocus, oldView, disableFocusMenu);
                    logger::debug("Unfocus: View [{}] unfocused during focus switch.", idToUnfocus);
                }

                viewData->ultralightView->Focus();
                PrismaUI::InputHandler::EnableInputCapture(viewId);

                if (disableFocusMenu) {
                    FocusMenu::Close();
                } else {
                    FocusMenu::Open();
                }

                QueueNativeFocusState(viewData, pauseGame);
                logger::debug("Focus: View [{}] focused successfully.", viewId);
            })) {
            return false;
        }

        return true;
    }

    void Unfocus(const Core::PrismaViewId& viewId) {
        if (!ViewManager::IsValid(viewId)) {
            logger::warn("Unfocus: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            std::shared_ptr<PrismaView> viewData = nullptr;
            {
                std::shared_lock lock(viewsMutex);
                auto it = views.find(viewId);
                if (it != views.end()) {
                    viewData = it->second;
                }
            }

            if (!viewData) {
                logger::warn("Unfocus: View [{}] not found during operation execution.", viewId);
                PrismaUI::InputHandler::DisableInputCapture(viewId);
                FocusMenu::Close();
                QueueNativeInputStateRefresh();
                return;
            }

            const bool hasUltralightFocus = viewData->ultralightView && viewData->ultralightView->HasFocus();
            const bool ownsInputCapture = PrismaUI::InputHandler::IsInputCaptureActiveForView(viewId);
            const bool ownsPause = viewData->isPaused.load(std::memory_order_acquire) ||
                                   viewData->nativePauseApplied.load(std::memory_order_acquire);

            if (!hasUltralightFocus && !ownsInputCapture && !ownsPause) {
                logger::debug("Unfocus: View [{}] has no remaining focus/capture state.", viewId);
                return;
            }

            PerformUnfocusOperations(viewId, viewData);
            logger::debug("Unfocus: View [{}] unfocused successfully.", viewId);
        });
    }

    bool HasFocus(const Core::PrismaViewId& viewId) {
        std::shared_ptr<PrismaView> viewData = nullptr;
        {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it != views.end()) {
                viewData = it->second;
            }
        }

        if (viewData && !viewData->isDestroying.load(std::memory_order_acquire)) {
            auto future = ultralightThread.submit(
                [view_ptr = viewData->ultralightView]() -> bool { return view_ptr ? view_ptr->HasFocus() : false; });
            try {
                return future.get();
            } catch (const std::exception& e) {
                logger::error("Exception getting focus state for View [{}]: {}", viewId, e.what());
                return false;
            }
        } else {
            logger::warn("HasFocus: View ID [{}] not found.", viewId);
            return false;
        }
    }

    bool ViewHasInputFocus(const Core::PrismaViewId& viewId) {
        std::shared_ptr<PrismaView> viewData = nullptr;
        {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it != views.end()) {
                viewData = it->second;
            }
        }

        if (viewData && !viewData->isDestroying.load(std::memory_order_acquire) && viewData->ultralightView) {
            auto future = ultralightThread.submit([view_ptr = viewData->ultralightView]() -> bool {
                if (view_ptr) {
                    return view_ptr->HasInputFocus();
                }
                return false;
            });
            try {
                return future.get();
            } catch (const std::exception& e) {
                logger::error("View [{}]: Exception in ViewHasInputFocus: {}", viewId, e.what());
                return false;
            }
        }
        return false;
    }

    void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize) {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
            if (pixelSize <= 0) {
                logger::warn("SetScrollingPixelSize: Invalid pixel size {} for view [{}]. Must be > 0. Using default.",
                             pixelSize, viewId);
                it->second->scrollingPixelSize = 16;
            } else {
                it->second->scrollingPixelSize = pixelSize;
                logger::debug("SetScrollingPixelSize: Set {} pixels per scroll line for view [{}]", pixelSize, viewId);
            }
        } else {
            logger::warn("SetScrollingPixelSize: View ID [{}] not found.", viewId);
        }
    }

    int GetScrollingPixelSize(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
            return it->second->scrollingPixelSize;
        }
        logger::warn("GetScrollingPixelSize: View ID [{}] not found, returning default.", viewId);
        return 28;
    }

    void Destroy(const Core::PrismaViewId& viewId) {
        logger::info("Destroy: Beginning destruction of View [{}]", viewId);

        std::shared_ptr<PrismaView> viewDataToDestroy = nullptr;
        {
            std::unique_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it == views.end() || !it->second) {
                logger::warn("Destroy: View ID [{}] not found.", viewId);
                return;
            }

            viewDataToDestroy = it->second;
            bool expected = false;
            if (!viewDataToDestroy->isDestroying.compare_exchange_strong(expected, true,
                                                                         std::memory_order_acq_rel)) {
                logger::debug("Destroy: View [{}] is already being destroyed.", viewId);
                return;
            }
            viewDataToDestroy->isHidden.store(true, std::memory_order_release);
        }

        // Once destruction begins no new view operations are accepted. Clear anything that has not
        // started yet, then perform the final unfocus directly on the Ultralight worker using the
        // captured shared_ptr. This guarantees native input cleanup happens before the map entry can
        // disappear, instead of enqueueing Unfocus() and immediately invalidating it.
        ViewOperationQueue::ClearOperations(viewId);
        logger::debug("Destroy: Cleared pending operations for View [{}]", viewId);

        auto cleanupUltralight = [viewId, viewData = viewDataToDestroy]() {
            try {
                logger::debug("Destroy: Beginning Ultralight resources cleanup for View [{}]", viewId);

                PerformUnfocusOperations(viewId, viewData);

                if (viewData->inspectorView) {
                    logger::debug("Destroy: Releasing inspector view for View [{}]", viewId);
                    viewData->inspectorView = nullptr;
                }
                Inspector::DestroyInspectorResources(viewData.get());

                if (viewData->ultralightView) {
                    logger::debug("Destroy: Detaching listeners for View [{}]", viewId);
                    viewData->ultralightView->set_load_listener(nullptr);
                    viewData->ultralightView->set_view_listener(nullptr);

                    viewData->loadListener.reset();
                    viewData->viewListener.reset();

                    viewData->ultralightView = nullptr;
                    logger::debug("Destroy: Ultralight View object released for View [{}]", viewId);
                }

                {
                    std::lock_guard lock(viewData->bufferMutex);
                    viewData->pixelBuffer.clear();
                    viewData->pixelBuffer.shrink_to_fit();
                    viewData->bufferWidth = 0;
                    viewData->bufferHeight = 0;
                    viewData->bufferStride = 0;
                    logger::debug("Destroy: Pixel buffer cleared for View [{}]", viewId);
                }

                viewData->isLoadingFinished = false;
                viewData->newFrameReady = false;

                logger::debug("Destroy: Ultralight resources for View [{}] cleaned up successfully", viewId);
                return true;
            } catch (const std::exception& e) {
                logger::error("Destroy: Exception during Ultralight resource cleanup for View [{}]: {}", viewId,
                              e.what());
                return false;
            } catch (...) {
                logger::error("Destroy: Unknown exception during Ultralight resource cleanup for View [{}]", viewId);
                return false;
            }
        };

        bool ultralightSuccess = false;
        try {
            if (ultralightThread.IsWorkerThread()) {
                ultralightSuccess = cleanupUltralight();
            } else {
                ultralightSuccess = ultralightThread.submit(cleanupUltralight).get();
            }
        } catch (const std::exception& e) {
            logger::error("Destroy: Exception waiting for Ultralight cleanup for View [{}]: {}", viewId, e.what());
        }

        if (!ultralightSuccess) {
            logger::warn("Destroy: Ultralight resources cleanup reported failure for View [{}]", viewId);
        }

        {
            std::unique_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it != views.end() && it->second.get() == viewDataToDestroy.get()) {
                views.erase(it);
                logger::debug("Destroy: Removed View [{}] from views map", viewId);
            }
        }

        {
            std::lock_guard<std::mutex> lock(jsCallbacksMutex);
            logger::debug("Destroy: Removing JavaScript callbacks for View [{}]", viewId);

            auto it = jsCallbacks.begin();
            size_t removedCallbacks = 0;

            while (it != jsCallbacks.end()) {
                if (it->first.first == viewId) {
                    it = jsCallbacks.erase(it);
                    removedCallbacks++;
                } else {
                    ++it;
                }
            }

            if (removedCallbacks > 0) {
                logger::debug("Destroy: Removed {} JavaScript callback(s) for View [{}]", removedCallbacks, viewId);
            }
        }

        bool hasD3DResources = (viewDataToDestroy->texture != nullptr || viewDataToDestroy->textureView != nullptr);

        if (hasD3DResources) {
            logger::debug("Destroy: D3D resources present for View [{}], forcing manual cleanup", viewId);

            if (viewDataToDestroy->textureView) {
                logger::debug("Destroy: Releasing textureView for View [{}]", viewId);
                viewDataToDestroy->textureView->Release();
                viewDataToDestroy->textureView = nullptr;
            }

            if (viewDataToDestroy->texture) {
                logger::debug("Destroy: Releasing texture for View [{}]", viewId);
                viewDataToDestroy->texture->Release();
                viewDataToDestroy->texture = nullptr;
            }

            viewDataToDestroy->textureWidth = 0;
            viewDataToDestroy->textureHeight = 0;

            logger::debug("Destroy: D3D resources released for View [{}]", viewId);
        } else {
            logger::debug("Destroy: No D3D resources to release for View [{}]", viewId);
        }

        viewDataToDestroy->pendingResourceRelease = false;

        logger::info("Destroy: View [{}] successfully destroyed", viewId);
    }

    void SetOrder(const Core::PrismaViewId& viewId, int order) {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
            it->second->order = order;
            logger::debug("SetOrder: Set order {} for view [{}]", order, viewId);
        } else {
            logger::warn("SetOrder: View ID [{}] not found.", viewId);
        }
    }

    int GetOrder(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
            return it->second->order;
        }
        logger::warn("GetOrder: View ID [{}] not found, returning -1.", viewId);
        return -1;
    }

    // ========== Inspector API Wrappers ==========

    void CreateInspectorView(const Core::PrismaViewId& viewId) { Inspector::CreateInspectorView(viewId); }

    void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool visible) {
        Inspector::SetInspectorVisibility(viewId, visible);
    }

    bool IsInspectorVisible(const Core::PrismaViewId& viewId) { return Inspector::IsInspectorVisible(viewId); }

    void SetInspectorBounds(const Core::PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width,
                            uint32_t height) {
        Inspector::SetInspectorBounds(viewId, topLeftX, topLeftY, width, height);
    }

    bool HasAnyActiveFocus() {
        std::vector<ultralight::RefPtr<ultralight::View>> viewPtrs;
        {
            std::shared_lock lock(viewsMutex);
            for (const auto& pair : views) {
                if (pair.second && !pair.second->isDestroying.load(std::memory_order_acquire) &&
                    pair.second->ultralightView) {
                    viewPtrs.push_back(pair.second->ultralightView);
                }
            }
        }

        if (viewPtrs.empty()) {
            return false;
        }

        // Submit a single task to check all views on the UI thread
        auto future = ultralightThread.submit([viewPtrs = std::move(viewPtrs)]() -> bool {
            for (const auto& view_ptr : viewPtrs) {
                if (view_ptr && view_ptr->HasFocus()) {
                    return true;
                }
            }
            return false;
        });

        try {
            return future.get();
        } catch (const std::exception& e) {
            logger::error("HasAnyActiveFocus: Exception checking focus: {}", e.what());
            return false;
        }
    }

    void RegisterConsoleCallback(const Core::PrismaViewId& viewId,
                                 std::function<void(Core::PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback) {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
            it->second->consoleMessageCallback = std::move(callback);
        } else {
            logger::warn("RegisterConsoleCallback: View ID [{}] not found.", viewId);
        }
    }

    void RegisterTranslations(const Core::PrismaViewId& viewId, const std::string& pluginName) {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
            it->second->translationPluginName = pluginName;
            logger::info("RegisterTranslations: View [{}] registered translations for '{}'", viewId, pluginName);
        } else {
            logger::warn("RegisterTranslations: View ID [{}] not found.", viewId);
        }
    }

    void EnumerateViews(std::function<void(Core::PrismaViewId, const std::string&)> callback) {
        if (!callback) return;

        // Snapshot under shared lock so the callback runs outside the lock.
        std::vector<std::pair<Core::PrismaViewId, std::string>> snapshot;
        {
            std::shared_lock lock(viewsMutex);
            snapshot.reserve(views.size());
            for (const auto& [id, viewData] : views) {
                if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) continue;
                // Strip the "file:///views/" prefix to expose the original relative path.
                std::string path = viewData->originalUrl;
                constexpr std::string_view kPrefix = "file:///views/";
                if (path.rfind(kPrefix, 0) == 0)
                    path = path.substr(kPrefix.size());
                snapshot.emplace_back(id, std::move(path));
            }
        }

        for (const auto& [id, path] : snapshot)
            callback(id, path);
    }
}