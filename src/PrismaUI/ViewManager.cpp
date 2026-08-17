#include "ViewManager.h"

#include <algorithm>
#include <mutex>
#include <string_view>
#include <utility>

#include "Core.h"
#include "GameThreadDispatcher.h"
#include "InputHandler.h"
#include "Inspector.h"
#include "Listeners.h"
#include "RenderRetirement.h"
#include "ViewOperationQueue.h"

namespace PrismaUI::ViewManager {
using namespace Core;

namespace {

std::mutex g_initializeMutex;

std::shared_ptr<PrismaView> GetView(Core::PrismaViewId viewId, bool includeDestroying = false)
{
    std::shared_lock lock(viewsMutex);
    auto it = views.find(viewId);
    if (it == views.end() || !it->second) return nullptr;
    if (!includeDestroying && it->second->isDestroying.load(std::memory_order_acquire)) return nullptr;
    return it->second;
}

bool IsSafeViewPath(std::string_view path)
{
    if (path.empty() || path.size() > 1024) return false;
    if (path.front() == '/' || path.front() == '\\' || path.find(':') != std::string_view::npos) return false;

    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find_first_of("/\\", start);
        const std::string_view part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part == "..") return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

void RunSafetyTask(std::function<void()> task)
{
    if (!task) return;
    auto fallback = task;
    if (GameThreadDispatcher::DispatchSafety(std::move(task))) return;

    if (auto* tasks = F4SE::GetTaskInterface()) {
        logger::warn("Game-thread safety dispatch unavailable; using F4SE fallback");
        tasks->AddTask(std::move(fallback));
    }
}

void RefreshNativeInputState()
{
    RunSafetyTask([] {
        if (auto* controlMap = RE::ControlMap::GetSingleton()) {
            controlMap->SetIgnoreKeyboardMouse(InputHandler::IsAnyInputCaptureActive());
        }
    });
}

bool QueueNativeFocusState(const std::shared_ptr<PrismaView>& viewData, bool pauseGame)
{
    if (!viewData || !GameThreadDispatcher::IsReady()) return false;
    if (pauseGame) viewData->isPaused.store(true, std::memory_order_release);

    const bool accepted = GameThreadDispatcher::Dispatch([viewData, pauseGame] {
        if (viewData->isDestroying.load(std::memory_order_acquire)) return;

        if (pauseGame && viewData->isPaused.load(std::memory_order_acquire)) {
            bool expected = false;
            if (viewData->nativePauseApplied.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ++ui->freezeFramePause;
                } else {
                    viewData->nativePauseApplied.store(false, std::memory_order_release);
                }
            }
        }

        if (auto* controlMap = RE::ControlMap::GetSingleton()) {
            controlMap->SetIgnoreKeyboardMouse(InputHandler::IsAnyInputCaptureActive());
        }
    }, viewData->id);

    if (!accepted && pauseGame) viewData->isPaused.store(false, std::memory_order_release);
    return accepted;
}

void QueueNativeUnfocusState(const std::shared_ptr<PrismaView>& viewData)
{
    if (viewData) viewData->isPaused.store(false, std::memory_order_release);

    RunSafetyTask([viewData] {
        if (viewData && viewData->nativePauseApplied.exchange(false, std::memory_order_acq_rel)) {
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->freezeFramePause > 0) {
                --ui->freezeFramePause;
            }
        }

        if (auto* controlMap = RE::ControlMap::GetSingleton()) {
            controlMap->SetIgnoreKeyboardMouse(InputHandler::IsAnyInputCaptureActive());
        }
    });
}

void ReleaseNativeOwnership(Core::PrismaViewId viewId, const std::shared_ptr<PrismaView>& viewData,
                            bool closeFocusMenu)
{
    InputHandler::DisableInputCapture(viewId);
    InputHandler::ClearImeState(viewId);
    if (closeFocusMenu) FocusMenu::Close();
    if (viewData) QueueNativeUnfocusState(viewData);
    else RefreshNativeInputState();
}

void PerformUnfocusOperations(Core::PrismaViewId viewId, const std::shared_ptr<PrismaView>& viewData,
                              bool closeFocusMenu = true)
{
    ReleaseNativeOwnership(viewId, viewData, closeFocusMenu);
    if (viewData && viewData->ultralightView) viewData->ultralightView->Unfocus();
}

bool HasOwnedFocus(Core::PrismaViewId viewId, const std::shared_ptr<PrismaView>& viewData)
{
    if (!viewData) return false;
    const bool browserFocus = viewData->ultralightView && viewData->ultralightView->HasFocus();
    const bool inputFocus = InputHandler::IsInputCaptureActiveForView(viewId);
    const bool paused = viewData->isPaused.load(std::memory_order_acquire) ||
                        viewData->nativePauseApplied.load(std::memory_order_acquire);
    return browserFocus || inputFocus || paused;
}

void RemoveCallbacks(Core::PrismaViewId viewId)
{
    std::lock_guard lock(jsCallbacksMutex);
    for (auto it = jsCallbacks.begin(); it != jsCallbacks.end();) {
        if (it->first.first == viewId) it = jsCallbacks.erase(it);
        else ++it;
    }
}

void FinishDestroy(Core::PrismaViewId viewId, const std::shared_ptr<PrismaView>& viewData)
{
    {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second.get() == viewData.get()) views.erase(it);
    }

    viewData->pendingResourceRelease.store(true, std::memory_order_release);
    RenderRetirement::Enqueue(viewData);
    logger::info("View [{}] destroyed", viewId);
}

bool CleanupUltralightView(Core::PrismaViewId viewId, const std::shared_ptr<PrismaView>& viewData)
{
    try {
        if (viewData->ultralightView) viewData->ultralightView->Unfocus();

        viewData->inspectorView = nullptr;
        Inspector::ClearInspectorBuffers(viewData.get());

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

        viewData->isLoadingFinished.store(false, std::memory_order_release);
        viewData->newFrameReady.store(false, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        logger::error("View [{}] cleanup failed: {}", viewId, e.what());
    } catch (...) {
        logger::error("View [{}] cleanup failed", viewId);
    }
    return false;
}

}

Core::PrismaViewId Create(const std::string& htmlPath, std::function<void(Core::PrismaViewId)> onDomReadyCallback)
{
    if (!IsSafeViewPath(htmlPath)) {
        logger::error("[PrismaUI Security] rejected view path '{}'", htmlPath);
        return 0;
    }

    {
        std::lock_guard lock(g_initializeMutex);
        if (!coreInitialized.load(std::memory_order_acquire)) {
            coreInitialized.store(true, std::memory_order_release);
            try {
                Core::InitializeCoreSystem();
            } catch (...) {
                coreInitialized.store(false, std::memory_order_release);
                throw;
            }
        }
        if (!renderer) {
            coreInitialized.store(false, std::memory_order_release);
            throw std::runtime_error("PrismaUI renderer initialization failed");
        }
    }

    const Core::PrismaViewId viewId = generator.generate();
    auto viewData = std::make_shared<PrismaView>();
    viewData->id = viewId;
    viewData->htmlPathToLoad = "file:///views/" + htmlPath;
    viewData->originalUrl = viewData->htmlPathToLoad;
    viewData->domReadyCallback = std::move(onDomReadyCallback);

    {
        std::unique_lock lock(viewsMutex);
        int maxOrder = -1;
        for (const auto& [id, view] : views) {
            if (view) maxOrder = std::max(maxOrder, view->order);
        }
        viewData->order = maxOrder + 1;
        views[viewId] = viewData;
    }

    logger::info("View [{}] created for '{}'", viewId, htmlPath);
    return viewId;
}

void Show(const Core::PrismaViewId& viewId)
{
    if (!IsValid(viewId)) return;

    ViewOperationQueue::EnqueueOperation(viewId, [viewId] {
        auto viewData = GetView(viewId);
        if (viewData) viewData->isHidden.store(false, std::memory_order_release);
    });
}

void Hide(const Core::PrismaViewId& viewId)
{
    if (!IsValid(viewId)) return;

    ViewOperationQueue::EnqueueOperation(viewId, [viewId] {
        auto viewData = GetView(viewId);
        if (!viewData || viewData->isHidden.load(std::memory_order_acquire)) return;
        if (HasOwnedFocus(viewId, viewData)) PerformUnfocusOperations(viewId, viewData);
        viewData->isHidden.store(true, std::memory_order_release);
    });
}

bool IsHidden(const Core::PrismaViewId& viewId)
{
    auto viewData = GetView(viewId, true);
    return !viewData || viewData->isHidden.load(std::memory_order_acquire) ||
           viewData->isDestroying.load(std::memory_order_acquire);
}

bool IsValid(const Core::PrismaViewId& viewId)
{
    return static_cast<bool>(GetView(viewId));
}

bool Focus(const Core::PrismaViewId& viewId, bool pauseGame, bool disableFocusMenu)
{
    if (!IsValid(viewId) || !GameThreadDispatcher::IsReady()) {
        logger::warn("Focus refused for View [{}]: game-thread dispatcher is not ready", viewId);
        return false;
    }

    return ViewOperationQueue::EnqueueOperation(viewId, [viewId, pauseGame, disableFocusMenu] {
        auto viewData = GetView(viewId);
        if (!viewData || !viewData->ultralightView || viewData->isHidden.load(std::memory_order_acquire)) return;

        std::vector<std::pair<Core::PrismaViewId, std::shared_ptr<PrismaView>>> toUnfocus;
        {
            std::shared_lock lock(viewsMutex);
            for (const auto& [id, view] : views) {
                if (id == viewId || !view || view->isDestroying.load(std::memory_order_acquire)) continue;
                if ((view->ultralightView && view->ultralightView->HasFocus()) ||
                    InputHandler::IsInputCaptureActiveForView(id) || view->isPaused.load(std::memory_order_acquire) ||
                    view->nativePauseApplied.load(std::memory_order_acquire)) {
                    toUnfocus.emplace_back(id, view);
                }
            }
        }

        for (const auto& [id, oldView] : toUnfocus) {
            PerformUnfocusOperations(id, oldView, disableFocusMenu);
        }

        if (!viewData->ultralightView->HasFocus()) viewData->ultralightView->Focus();
        InputHandler::EnableInputCapture(viewId);
        if (disableFocusMenu) FocusMenu::Close();
        else FocusMenu::Open();

        if (!QueueNativeFocusState(viewData, pauseGame)) {
            InputHandler::DisableInputCapture(viewId);
            InputHandler::ClearImeState(viewId);
            viewData->ultralightView->Unfocus();
            FocusMenu::Close();
            viewData->isPaused.store(false, std::memory_order_release);
            RefreshNativeInputState();
            logger::error("Focus rollback for View [{}]: native state dispatch failed", viewId);
        }
    });
}

void Unfocus(const Core::PrismaViewId& viewId)
{
    if (!IsValid(viewId)) return;

    ViewOperationQueue::EnqueueOperation(viewId, [viewId] {
        auto viewData = GetView(viewId);
        if (!viewData || !HasOwnedFocus(viewId, viewData)) return;
        PerformUnfocusOperations(viewId, viewData);
    });
}

bool HasFocus(const Core::PrismaViewId& viewId)
{
    auto viewData = GetView(viewId);
    if (!viewData) return false;

    auto check = [viewData] {
        return !viewData->isDestroying.load(std::memory_order_acquire) && viewData->ultralightView &&
               viewData->ultralightView->HasFocus();
    };

    if (ultralightThread.IsWorkerThread()) return check();
    try {
        return ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::HIGH, check).get();
    } catch (...) {
        return false;
    }
}

bool ViewHasInputFocus(const Core::PrismaViewId& viewId)
{
    auto viewData = GetView(viewId);
    if (!viewData) return false;

    auto check = [viewData] {
        return !viewData->isDestroying.load(std::memory_order_acquire) && viewData->ultralightView &&
               viewData->ultralightView->HasInputFocus();
    };

    if (ultralightThread.IsWorkerThread()) return check();
    try {
        return ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::HIGH, check).get();
    } catch (...) {
        return false;
    }
}

void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize)
{
    if (pixelSize <= 0) pixelSize = 16;
    std::unique_lock lock(viewsMutex);
    auto it = views.find(viewId);
    if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
        it->second->scrollingPixelSize = pixelSize;
    }
}

int GetScrollingPixelSize(const Core::PrismaViewId& viewId)
{
    auto viewData = GetView(viewId);
    return viewData ? viewData->scrollingPixelSize : 28;
}

void Destroy(const Core::PrismaViewId& viewId)
{
    std::shared_ptr<PrismaView> viewData;
    {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it == views.end() || !it->second) return;
        viewData = it->second;

        bool expected = false;
        if (!viewData->isDestroying.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        viewData->isHidden.store(true, std::memory_order_release);
    }

    GameThreadDispatcher::DropView(viewId);
    ViewOperationQueue::ClearOperations(viewId);
    RemoveCallbacks(viewId);
    ReleaseNativeOwnership(viewId, viewData, true);

    auto cleanup = [viewId, viewData] {
        CleanupUltralightView(viewId, viewData);
        FinishDestroy(viewId, viewData);
    };

    if (ultralightThread.IsWorkerThread()) {
        try {
            ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::HIGH, std::move(cleanup));
        } catch (const std::exception& e) {
            logger::error("Deferred destroy dispatch failed for View [{}]: {}", viewId, e.what());
        }
        return;
    }

    try {
        ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::HIGH, std::move(cleanup)).get();
    } catch (const std::exception& e) {
        logger::error("Destroy failed for View [{}]: {}", viewId, e.what());
    }
}

void SetOrder(const Core::PrismaViewId& viewId, int order)
{
    std::unique_lock lock(viewsMutex);
    auto it = views.find(viewId);
    if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
        it->second->order = order;
    }
}

int GetOrder(const Core::PrismaViewId& viewId)
{
    auto viewData = GetView(viewId);
    return viewData ? viewData->order : -1;
}

void CreateInspectorView(const Core::PrismaViewId& viewId)
{
    Inspector::CreateInspectorView(viewId);
}

void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool visible)
{
    Inspector::SetInspectorVisibility(viewId, visible);
}

bool IsInspectorVisible(const Core::PrismaViewId& viewId)
{
    return Inspector::IsInspectorVisible(viewId);
}

void SetInspectorBounds(const Core::PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width,
                        uint32_t height)
{
    Inspector::SetInspectorBounds(viewId, topLeftX, topLeftY, width, height);
}

bool HasAnyActiveFocus()
{
    std::vector<std::shared_ptr<PrismaView>> snapshot;
    {
        std::shared_lock lock(viewsMutex);
        for (const auto& [id, view] : views) {
            if (view && !view->isDestroying.load(std::memory_order_acquire)) snapshot.push_back(view);
        }
    }

    auto check = [snapshot = std::move(snapshot)] {
        for (const auto& view : snapshot) {
            if (!view->isDestroying.load(std::memory_order_acquire) && view->ultralightView &&
                view->ultralightView->HasFocus()) {
                return true;
            }
        }
        return false;
    };

    if (ultralightThread.IsWorkerThread()) return check();
    try {
        return ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::HIGH, std::move(check)).get();
    } catch (...) {
        return false;
    }
}

void RegisterConsoleCallback(
    const Core::PrismaViewId& viewId,
    std::function<void(Core::PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback)
{
    std::unique_lock lock(viewsMutex);
    auto it = views.find(viewId);
    if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
        it->second->consoleMessageCallback = std::move(callback);
    }
}

void RegisterTranslations(const Core::PrismaViewId& viewId, const std::string& pluginName)
{
    std::unique_lock lock(viewsMutex);
    auto it = views.find(viewId);
    if (it != views.end() && it->second && !it->second->isDestroying.load(std::memory_order_acquire)) {
        it->second->translationPluginName = pluginName;
    }
}

void EnumerateViews(std::function<void(Core::PrismaViewId, const std::string&)> callback)
{
    if (!callback) return;

    std::vector<std::pair<Core::PrismaViewId, std::string>> snapshot;
    {
        std::shared_lock lock(viewsMutex);
        snapshot.reserve(views.size());
        for (const auto& [id, view] : views) {
            if (!view || view->isDestroying.load(std::memory_order_acquire)) continue;
            std::string path = view->originalUrl;
            constexpr std::string_view prefix = "file:///views/";
            if (path.rfind(prefix, 0) == 0) path = path.substr(prefix.size());
            snapshot.emplace_back(id, std::move(path));
        }
    }

    for (const auto& [id, path] : snapshot) callback(id, path);
}

}
