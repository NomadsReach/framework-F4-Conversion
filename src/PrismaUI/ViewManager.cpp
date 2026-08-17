#include "ViewManager.h"

#include <unordered_set>

#include "Core.h"
#include "InputHandler.h"
#include "Inspector.h"
#include "Listeners.h"
#include "ViewOperationQueue.h"

namespace PrismaUI::ViewManager {
    using namespace Core;

    static std::mutex issuedViewIdsMutex;
    static std::unordered_set<Core::PrismaViewId> issuedViewIds;

    static std::shared_ptr<PrismaView> FindView(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        return it != views.end() ? it->second : nullptr;
    }

    static std::shared_ptr<PrismaView> FindLiveView(const Core::PrismaViewId& viewId) {
        auto viewData = FindView(viewId);
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return viewData;
    }

    static Core::PrismaViewId GenerateViewId() {
        std::lock_guard lock(issuedViewIdsMutex);
        while (true) {
            const auto viewId = generator.generate();
            if (viewId != 0 && issuedViewIds.insert(viewId).second) {
                return viewId;
            }
        }
    }

    Core::PrismaViewId Create(const std::string& htmlPath, std::function<void(Core::PrismaViewId)> onDomReadyCallback) {
        bool expectedInit = false;
        if (coreInitialized.compare_exchange_strong(expectedInit, true)) {
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

        if (htmlPath.substr(0, 7) == "http://" || htmlPath.substr(0, 8) == "https://") {
            logger::error("[PrismaUI Security] CreateView blocked: external URL '{}' is not permitted. "
                          "Only local file paths relative to Data/PrismaUI_F4/views/ are allowed.",
                          htmlPath);
            return 0;
        }

        const Core::PrismaViewId newViewId = GenerateViewId();
        std::string fileUrl = "file:///views/" + htmlPath;
        auto viewData = std::make_shared<Core::PrismaView>();
        viewData->id = newViewId;
        viewData->ultralightView = nullptr;
        viewData->htmlPathToLoad = fileUrl;
        viewData->originalUrl = fileUrl;
        viewData->isHidden = false;
        viewData->domReadyCallback = std::move(onDomReadyCallback);

        {
            std::unique_lock lock(viewsMutex);
            int maxOrder = -1;
            for (const auto& pair : views) {
                if (pair.second && pair.second->order > maxOrder) {
                    maxOrder = pair.second->order;
                }
            }
            viewData->order = maxOrder + 1;
            views.emplace(newViewId, viewData);
        }

        logger::info(
            "View [{}] creation requested for path: {} with order <{}>. Actual view will be created by UI thread.",
            newViewId, fileUrl, viewData->order);
        return newViewId;
    }

    static void PerformUnfocusOperations(const Core::PrismaViewId& viewId, const std::shared_ptr<PrismaView>& viewData,
                                         bool closeFocusMenu = true) {
        if (!viewData) {
            return;
        }

        if (viewData->isPaused.exchange(false)) {
            auto ui = RE::UI::GetSingleton();
            if (ui && ui->freezeFramePause > 0) {
                ui->freezeFramePause--;
            }
        }

        PrismaUI::InputHandler::DisableInputCapture(viewId);
        if (closeFocusMenu) {
            PrismaUI::InputHandler::ClearImeState(viewId);
        }
        if (viewData->ultralightView) {
            viewData->ultralightView->Unfocus();
        }
        if (closeFocusMenu) {
            FocusMenu::Close();
        }

        if (auto controlMap = RE::ControlMap::GetSingleton()) {
            controlMap->SetIgnoreKeyboardMouse(PrismaUI::InputHandler::IsAnyInputCaptureActive());
        }
    }

    void Show(const Core::PrismaViewId& viewId) {
        if (!IsValid(viewId)) {
            logger::warn("Show: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            auto viewData = FindLiveView(viewId);
            if (!viewData) {
                return;
            }
            if (!viewData->isHidden.load()) {
                logger::debug("Show: View [{}] is already visible.", viewId);
                return;
            }
            viewData->isHidden = false;
            logger::debug("View [{}] marked as Visible.", viewId);
        });
    }

    void Hide(const Core::PrismaViewId& viewId) {
        if (!IsValid(viewId)) {
            logger::warn("Hide: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            auto viewData = FindLiveView(viewId);
            if (!viewData) {
                return;
            }
            if (viewData->isHidden.load()) {
                logger::debug("Hide: View [{}] is already hidden.", viewId);
                return;
            }
            if (viewData->ultralightView && viewData->ultralightView->HasFocus()) {
                PerformUnfocusOperations(viewId, viewData);
            }
            viewData->isHidden = true;
            logger::debug("View [{}] marked as Hidden.", viewId);
        });
    }

    bool IsHidden(const Core::PrismaViewId& viewId) {
        auto viewData = FindLiveView(viewId);
        if (viewData) {
            return viewData->isHidden.load();
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
        if (!IsValid(viewId)) {
            logger::warn("Focus: View ID [{}] not found.", viewId);
            return false;
        }

        return ViewOperationQueue::EnqueueOperation(viewId, [viewId, pauseGame, disableFocusMenu]() {
            auto viewData = FindLiveView(viewId);
            if (!viewData || !viewData->ultralightView) {
                logger::warn("Focus: View [{}] or its Ultralight View is not ready.", viewId);
                return;
            }
            if (viewData->isHidden.load()) {
                logger::warn("Focus: View [{}] is hidden, cannot focus.", viewId);
                return;
            }
            if (viewData->ultralightView->HasFocus()) {
                logger::debug("Focus: View [{}] already has focus.", viewId);
                return;
            }

            std::vector<Core::PrismaViewId> viewsToUnfocus;
            {
                std::shared_lock lock(viewsMutex);
                for (const auto& pair : views) {
                    if (pair.first != viewId && pair.second &&
                        !pair.second->isDestroying.load(std::memory_order_acquire) && pair.second->ultralightView &&
                        pair.second->ultralightView->HasFocus()) {
                        viewsToUnfocus.push_back(pair.first);
                    }
                }
            }

            for (const auto idToUnfocus : viewsToUnfocus) {
                ViewOperationQueue::EnqueueOperation(idToUnfocus, [idToUnfocus]() {
                    auto otherView = FindLiveView(idToUnfocus);
                    if (otherView && otherView->ultralightView) {
                        PerformUnfocusOperations(idToUnfocus, otherView, false);
                        logger::debug("Unfocus: View [{}] unfocused (focus switching).", idToUnfocus);
                    }
                });
            }

            viewData->ultralightView->Focus();
            PrismaUI::InputHandler::EnableInputCapture(viewId);
            if (!disableFocusMenu) {
                FocusMenu::Open();
            }
            if (auto controlMap = RE::ControlMap::GetSingleton()) {
                controlMap->SetIgnoreKeyboardMouse(true);
            }

            if (pauseGame) {
                auto ui = RE::UI::GetSingleton();
                if (ui) {
                    ui->freezeFramePause++;
                    viewData->isPaused.store(true);
                    logger::debug("Game paused for View [{}]", viewId);
                }
            }

            logger::debug("Focus: View [{}] focused successfully.", viewId);
        });
    }

    void Unfocus(const Core::PrismaViewId& viewId) {
        if (!IsValid(viewId)) {
            logger::warn("Unfocus: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            auto viewData = FindLiveView(viewId);
            if (!viewData) {
                return;
            }
            PerformUnfocusOperations(viewId, viewData);
            logger::debug("Unfocus: View [{}] unfocused successfully.", viewId);
        });
    }

    bool HasFocus(const Core::PrismaViewId& viewId) {
        auto viewData = FindLiveView(viewId);
        if (!viewData) {
            logger::warn("HasFocus: View ID [{}] not found.", viewId);
            return false;
        }

        auto checkFocus = [viewPtr = viewData->ultralightView]() -> bool {
            return viewPtr ? viewPtr->HasFocus() : false;
        };

        try {
            if (ultralightThread.IsWorkerThread()) {
                return checkFocus();
            }
            return ultralightThread.submit(checkFocus).get();
        } catch (const std::exception& e) {
            logger::error("Exception getting focus state for View [{}]: {}", viewId, e.what());
            return false;
        }
    }

    bool ViewHasInputFocus(const Core::PrismaViewId& viewId) {
        auto viewData = FindLiveView(viewId);
        if (!viewData || !viewData->ultralightView) {
            return false;
        }

        auto checkFocus = [viewPtr = viewData->ultralightView]() -> bool {
            return viewPtr ? viewPtr->HasInputFocus() : false;
        };

        try {
            if (ultralightThread.IsWorkerThread()) {
                return checkFocus();
            }
            return ultralightThread.submit(checkFocus).get();
        } catch (const std::exception& e) {
            logger::error("View [{}]: Exception in ViewHasInputFocus: {}", viewId, e.what());
            return false;
        }
    }

    void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize) {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) {
            logger::warn("SetScrollingPixelSize: View ID [{}] not found.", viewId);
            return;
        }

        if (pixelSize <= 0) {
            logger::warn("SetScrollingPixelSize: Invalid pixel size {} for view [{}]. Must be > 0. Using default.",
                         pixelSize, viewId);
            it->second->scrollingPixelSize = 16;
        } else {
            it->second->scrollingPixelSize = pixelSize;
            logger::debug("SetScrollingPixelSize: Set {} pixels per scroll line for view [{}]", pixelSize, viewId);
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

        auto viewDataToDestroy = FindView(viewId);
        if (!viewDataToDestroy) {
            logger::warn("Destroy: View ID [{}] not found.", viewId);
            return;
        }

        {
            std::lock_guard lock(viewDataToDestroy->operationMutex);
            bool expected = false;
            if (!viewDataToDestroy->isDestroying.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                logger::debug("Destroy: View [{}] destruction is already in progress", viewId);
                return;
            }
        }

        ViewOperationQueue::ClearOperations(viewId);

        auto releaseFocus = [viewId, viewData = viewDataToDestroy]() {
            PerformUnfocusOperations(viewId, viewData);
        };

        try {
            if (ultralightThread.IsWorkerThread()) {
                releaseFocus();
            } else {
                ultralightThread.submit(releaseFocus).get();
            }
        } catch (const std::exception& e) {
            logger::error("Destroy: Exception releasing focus for View [{}]: {}", viewId, e.what());
            if (viewDataToDestroy->isPaused.exchange(false)) {
                if (auto ui = RE::UI::GetSingleton(); ui && ui->freezeFramePause > 0) {
                    ui->freezeFramePause--;
                }
            }
            PrismaUI::InputHandler::DisableInputCapture(viewId);
            FocusMenu::Close();
            if (auto controlMap = RE::ControlMap::GetSingleton()) {
                controlMap->SetIgnoreKeyboardMouse(PrismaUI::InputHandler::IsAnyInputCaptureActive());
            }
        }

        {
            std::unique_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it == views.end() || it->second != viewDataToDestroy) {
                logger::warn("Destroy: View ID [{}] changed during destruction.", viewId);
                return;
            }
            views.erase(it);
        }

        viewDataToDestroy->isHidden = true;

        {
            std::lock_guard<std::mutex> lock(jsCallbacksMutex);
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

        auto cleanupUltralight = [viewId, viewData = viewDataToDestroy]() -> bool {
            try {
                if (viewData->inspectorView) {
                    viewData->inspectorView = nullptr;
                }
                Inspector::DestroyInspectorResources(viewData.get());

                if (viewData->ultralightView) {
                    viewData->ultralightView->set_load_listener(nullptr);
                    viewData->ultralightView->set_view_listener(nullptr);
                    viewData->loadListener.reset();
                    viewData->viewListener.reset();
                    viewData->ultralightView = nullptr;
                }

                {
                    std::lock_guard lock(viewData->bufferMutex);
                    viewData->pixelBuffer.clear();
                    viewData->pixelBuffer.shrink_to_fit();
                    viewData->bufferWidth = 0;
                    viewData->bufferHeight = 0;
                    viewData->bufferStride = 0;
                }

                viewData->isLoadingFinished = false;
                viewData->newFrameReady = false;
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

        try {
            const bool success = ultralightThread.IsWorkerThread() ? cleanupUltralight()
                                                                   : ultralightThread.submit(cleanupUltralight).get();
            if (!success) {
                logger::warn("Destroy: Ultralight resources cleanup reported failure for View [{}]", viewId);
            }
        } catch (const std::exception& e) {
            logger::error("Destroy: Exception waiting for Ultralight cleanup for View [{}]: {}", viewId, e.what());
        }

        const bool hasD3DResources = viewDataToDestroy->texture != nullptr || viewDataToDestroy->textureView != nullptr;
        if (hasD3DResources) {
            if (viewDataToDestroy->textureView) {
                viewDataToDestroy->textureView->Release();
                viewDataToDestroy->textureView = nullptr;
            }
            if (viewDataToDestroy->texture) {
                viewDataToDestroy->texture->Release();
                viewDataToDestroy->texture = nullptr;
            }
            viewDataToDestroy->textureWidth = 0;
            viewDataToDestroy->textureHeight = 0;
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

    void CreateInspectorView(const Core::PrismaViewId& viewId) {
        if (IsValid(viewId)) {
            Inspector::CreateInspectorView(viewId);
        }
    }

    void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool visible) {
        if (IsValid(viewId)) {
            Inspector::SetInspectorVisibility(viewId, visible);
        }
    }

    bool IsInspectorVisible(const Core::PrismaViewId& viewId) {
        return IsValid(viewId) && Inspector::IsInspectorVisible(viewId);
    }

    void SetInspectorBounds(const Core::PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width,
                            uint32_t height) {
        if (IsValid(viewId)) {
            Inspector::SetInspectorBounds(viewId, topLeftX, topLeftY, width, height);
        }
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

        auto checkFocus = [viewPtrs = std::move(viewPtrs)]() -> bool {
            for (const auto& viewPtr : viewPtrs) {
                if (viewPtr && viewPtr->HasFocus()) {
                    return true;
                }
            }
            return false;
        };

        try {
            if (ultralightThread.IsWorkerThread()) {
                return checkFocus();
            }
            return ultralightThread.submit(checkFocus).get();
        } catch (const std::exception& e) {
            logger::error("HasAnyActiveFocus: Exception checking focus: {}", e.what());
            return false;
        }
    }

    void RegisterConsoleCallback(
        const Core::PrismaViewId& viewId,
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
        if (!callback) {
            return;
        }

        std::vector<std::pair<Core::PrismaViewId, std::string>> snapshot;
        {
            std::shared_lock lock(viewsMutex);
            snapshot.reserve(views.size());
            for (const auto& [id, viewData] : views) {
                if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) {
                    continue;
                }
                std::string path = viewData->originalUrl;
                constexpr std::string_view kPrefix = "file:///views/";
                if (path.rfind(kPrefix, 0) == 0) {
                    path = path.substr(kPrefix.size());
                }
                snapshot.emplace_back(id, std::move(path));
            }
        }

        for (const auto& [id, path] : snapshot) {
            callback(id, path);
        }
    }
}
