#include "Core.h"

#include <cstdio>
#include <eh.h>
#include <psapi.h>

#include "Communication.h"
#include "InputHandler.h"
#include "Inspector.h"
#include "Listeners.h"
#include "Utils/DllLoader.h"
#include "ViewManager.h"
#include "ViewOperationQueue.h"
#include "ViewRenderer.h"

namespace {
    class SEHException : public std::exception {
    public:
        // EXCEPTION_POINTERS is only valid during the translator call, so copy the fields we log later.
        SEHException(unsigned int code, EXCEPTION_POINTERS* exceptionPointers)
            : code_(code), address_(nullptr), accessType_(0), accessAddress_(0) {
            if (!exceptionPointers || !exceptionPointers->ExceptionRecord) {
                return;
            }

            address_ = exceptionPointers->ExceptionRecord->ExceptionAddress;
            if (code == EXCEPTION_ACCESS_VIOLATION && exceptionPointers->ExceptionRecord->NumberParameters >= 2) {
                accessType_ = exceptionPointers->ExceptionRecord->ExceptionInformation[0];
                accessAddress_ = exceptionPointers->ExceptionRecord->ExceptionInformation[1];
            }
        }

        const char* what() const noexcept override { return "Windows Structured Exception"; }
        unsigned int code() const { return code_; }
        void* address() const { return address_; }

        std::string details() const {
            switch (code_) {
                case EXCEPTION_ACCESS_VIOLATION: {
                    const char* operation = accessType_ == 0 ? "read" : accessType_ == 8 ? "execute" : "write";
                    char buffer[128];
                    snprintf(buffer, sizeof(buffer), "Access Violation (%s at 0x%p)", operation,
                             reinterpret_cast<void*>(accessAddress_));
                    return buffer;
                }
                case EXCEPTION_STACK_OVERFLOW:
                    return "Stack Overflow";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                    return "Integer Divide by Zero";
                default: {
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "Code 0x%08X", code_);
                    return buffer;
                }
            }
        }

    private:
        unsigned int code_;
        void* address_;
        ULONG_PTR accessType_;
        ULONG_PTR accessAddress_;
    };

    void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* exceptionPointers) {
        // Throwing during stack overflow is unsafe because there may be no stack left to unwind.
        if (code == EXCEPTION_STACK_OVERFLOW) {
            return;
        }
        throw SEHException(code, exceptionPointers);
    }
}

namespace PrismaUI::Core {
    using namespace PrismaUI::InputHandler;
    using namespace PrismaUI::Listeners;
    using namespace PrismaUI::ViewManager;
    using namespace PrismaUI::ViewRenderer;

    SingleThreadExecutor ultralightThread;
    NanoIdGenerator generator;
    std::atomic<bool> coreInitialized = false;
    std::atomic<bool> rendererInitFailed = false;

    // Ultralight stores the platform logger as a non-owning pointer.
    static std::unique_ptr<MyUltralightLogger> ultralightLogger;

    RefPtr<Renderer> renderer;
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    HWND hWnd = nullptr;

    ScreenSize screenSize;

    std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
    std::unique_ptr<DirectX::CommonStates> commonStates;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorTexture;

    std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
    std::shared_mutex viewsMutex;

    std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> jsCallbacks;
    std::mutex jsCallbacksMutex;

    PrismaView::~PrismaView() {
        ViewRenderer::ReleaseViewTexture(this);
        Inspector::ReleaseInspectorTexture(this);
    }

    void InitializeCoreSystem() {
        logger::info("Initializing PrismaUI Core System...");
        InitHooks();

        const auto basePath = Utils::GetBasePath();
        ultralightThread
            .submit([basePath]() {
                try {
                    Platform& platform = Platform::instance();
                    ultralightLogger = std::make_unique<MyUltralightLogger>();
                    platform.set_logger(ultralightLogger.get());
                    platform.set_font_loader(GetPlatformFontLoader());
                    platform.set_file_system(GetPlatformFileSystem(basePath.string().c_str()));

                    Config config;
                    config.resource_path_prefix = "resources/";

                    // Bound per-view JSC/WebCore memory so it does not crowd Fallout 4's own UI allocators.
                    config.override_ram_size = 1024u * 1024u * 1024u;
                    config.min_large_heap_size = 8u * 1024u * 1024u;
                    config.min_small_heap_size = 512u * 1024u;
                    config.memory_cache_size = 32u * 1024u * 1024u;
                    config.page_cache_size = 0;
                    config.num_renderer_threads = 2;
                    platform.set_config(config);

                    renderer = Renderer::Create();
                    if (!renderer) {
                        logger::critical("Failed to create Ultralight Renderer!");
                        rendererInitFailed = true;
                    } else {
                        logger::info("Ultralight Platform configured and Renderer created on UI thread.");
                    }
                } catch (const std::exception& e) {
                    logger::critical("Exception during Ultralight Platform/Renderer init on UI thread: {}", e.what());
                    rendererInitFailed = true;
                } catch (...) {
                    logger::critical("Unknown exception during Ultralight Platform/Renderer init on UI thread.");
                    rendererInitFailed = true;
                }
            })
            .get();

        auto ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::critical("PrismaUI Core initialization failed: RE::UI singleton is null.");
            rendererInitFailed = true;
            return;
        }
        ui->RegisterMenu(FocusMenu::MENU_NAME.data(), FocusMenu::Creator);

        logger::info("PrismaUI Core System Initialized.");
    }

    void InitHooks() {
        logger::debug("InitHooks: D3D hooks are installed from the kGameDataReady handler.");
    }

    void InitGraphics() {
        auto* rendererData = RE::BSGraphics::GetRendererData();
        if (!rendererData) {
            logger::critical("InitGraphics: BSGraphics::RendererData is null!");
            return;
        }

        if (!d3dDevice) {
            d3dDevice = reinterpret_cast<ID3D11Device*>(rendererData->device);
        }
        if (!d3dContext) {
            d3dContext = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
        }

        if (!hWnd && rendererData->renderWindow[0].hwnd) {
            hWnd = reinterpret_cast<HWND>(rendererData->renderWindow[0].hwnd);
            screenSize.width = static_cast<std::uint32_t>(rendererData->renderWindow[0].windowWidth);
            screenSize.height = static_cast<std::uint32_t>(rendererData->renderWindow[0].windowHeight);

            static std::atomic<bool> inputHandlerInitialized = false;
            bool expected = false;
            if (inputHandlerInitialized.compare_exchange_strong(expected, true)) {
                Initialize(hWnd, &ultralightThread, &views, &viewsMutex);

                logger::info("Attempting to install WndProc hook from render thread...");
                if (InstallWndProcHook()) {
                    logger::info("WndProc hook installed successfully from render thread.");
                } else {
                    logger::warn("Direct installation failed, scheduling on main thread...");
                    F4SE::GetTaskInterface()->AddTask([]() {
                        logger::info("Attempting to install WndProc hook from main thread...");
                        if (InstallWndProcHook()) {
                            logger::info("WndProc hook installed from main thread.");
                        } else {
                            logger::error("Failed to install WndProc hook!");
                        }
                    });
                }
            }
        } else if (!hWnd) {
            logger::warn("InitGraphics: Could not obtain HWND from RendererData.");
        }

        if (!d3dDevice || !d3dContext) {
            return;
        }

        if (!commonStates || !spriteBatch) {
            try {
                commonStates = std::make_unique<DirectX::CommonStates>(d3dDevice);
                spriteBatch = std::make_unique<DirectX::SpriteBatch>(d3dContext);
                logger::info("DirectXTK SpriteBatch and CommonStates initialized.");
            } catch (const std::exception& e) {
                logger::critical("Failed to initialize DirectXTK: {}", e.what());
                commonStates.reset();
                spriteBatch.reset();
            }
        }

        if (!cursorTexture) {
            auto cursorPath = Utils::GetBasePath() / "misc" / "cursor.png";
            HRESULT result =
                DirectX::CreateWICTextureFromFile(d3dDevice, cursorPath.wstring().c_str(), nullptr, &cursorTexture);
            if (SUCCEEDED(result)) {
                logger::info("Cursor texture loaded successfully.");
            } else {
                logger::error("Failed to load cursor texture from '{}'. HRESULT: 0x{:08X}", cursorPath.string(),
                              static_cast<unsigned int>(result));
                cursorTexture.Reset();
            }
        }
    }

    void RunWatchdog() {
        constexpr auto interval = std::chrono::seconds(30);
        static auto lastRun = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRun < interval) {
            return;
        }
        lastRun = now;

        std::vector<std::shared_ptr<PrismaView>> snapshot;
        {
            std::shared_lock lock(viewsMutex);
            snapshot.reserve(views.size());
            for (const auto& [viewId, viewData] : views) {
                if (viewData && !viewData->isDestroying.load(std::memory_order_acquire)) {
                    snapshot.push_back(viewData);
                }
            }
        }
        if (snapshot.empty()) {
            return;
        }

        std::size_t workingSetMB = 0;
        PROCESS_MEMORY_COUNTERS memoryCounters{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memoryCounters, sizeof(memoryCounters))) {
            workingSetMB = memoryCounters.WorkingSetSize / (1024 * 1024);
        }

        int visible = 0;
        int hidden = 0;
        int quarantined = 0;
        int faulting = 0;
        for (const auto& viewData : snapshot) {
            if (viewData->quarantined.load()) {
                ++quarantined;
            }
            if (viewData->isHidden.load()) {
                ++hidden;
            } else {
                ++visible;
            }
            if (viewData->faultCount.load() > 0) {
                ++faulting;
            }
        }

        logger::info("[Watchdog] views={} (visible={} hidden={} quarantined={}) faulting={} workingSet={}MB",
                     snapshot.size(), visible, hidden, quarantined, faulting, workingSetMB);

        for (const auto& viewData : snapshot) {
            if (viewData->faultCount.load() > 0 || viewData->quarantined.load()) {
                logger::warn("[Watchdog]   view[{}] url='{}' hidden={} faults={} quarantined={}", viewData->id,
                             viewData->originalUrl, viewData->isHidden.load(), viewData->faultCount.load(),
                             viewData->quarantined.load());
            }
        }

        constexpr std::size_t pressureMB = 6000;
        if (workingSetMB <= pressureMB) {
            return;
        }

        int reclaimed = 0;
        for (const auto& viewData : snapshot) {
            if (viewData->isDestroying.load(std::memory_order_acquire) || !viewData->isHidden.load() ||
                viewData->quarantined.load()) {
                continue;
            }

            bool freed = false;
            // GPU resources are released by the present-thread path, not from the watchdog's caller.
            if (viewData->texture || viewData->textureView) {
                viewData->pendingResourceRelease = true;
                freed = true;
            }

            {
                std::lock_guard bufferLock(viewData->bufferMutex);
                if (!viewData->isDestroying.load(std::memory_order_acquire) && !viewData->pixelBuffer.empty()) {
                    viewData->pixelBuffer.clear();
                    viewData->pixelBuffer.shrink_to_fit();
                    viewData->bufferWidth = 0;
                    viewData->bufferHeight = 0;
                    viewData->bufferStride = 0;
                    viewData->newFrameReady = false;
                    freed = true;
                }
            }

            if (freed) {
                ++reclaimed;
            }
        }

        if (reclaimed > 0) {
            logger::warn("[Watchdog] memory pressure ({}MB > {}MB): reclaimed render buffers for {} hidden view(s)",
                         workingSetMB, pressureMB, reclaimed);
        }
    }

    void D3DPresent(uint32_t presentArg) {
        (void)presentArg;

        if (!coreInitialized || rendererInitFailed) {
            return;
        }

        if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) {
            InitGraphics();
            if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) {
                return;
            }
        }

        std::vector<std::shared_ptr<PrismaView>> pendingReleases;
        {
            std::shared_lock lock(viewsMutex);
            pendingReleases.reserve(views.size());
            for (const auto& pair : views) {
                if (pair.second && pair.second->pendingResourceRelease.exchange(false)) {
                    pendingReleases.push_back(pair.second);
                }
            }
        }

        for (const auto& viewData : pendingReleases) {
            logger::debug("D3DPresent: Releasing D3D resources for View [{}] from render thread", viewData->id);
            ViewRenderer::ReleaseViewTexture(viewData.get());
            Inspector::ReleaseInspectorTexture(viewData.get());
        }

        ViewOperationQueue::ProcessAllViewOperations();

        auto ultralightFuture = ultralightThread.submit([]() {
            static bool sehTranslatorSet = false;
            if (!sehTranslatorSet) {
                _set_se_translator(SEHTranslator);
                sehTranslatorSet = true;
            }

            try {
                auto localRenderer = renderer;
                if (!localRenderer) {
                    logger::warn("UI Thread: Renderer is null, skipping frame.");
                    return;
                }

                std::vector<std::shared_ptr<PrismaView>> viewsToRecover;
                {
                    std::shared_lock lock(viewsMutex);
                    for (const auto& pair : views) {
                        const auto& viewData = pair.second;
                        if (viewData && !viewData->isDestroying.load(std::memory_order_acquire) &&
                            viewData->needsRecovery.load() && viewData->ultralightView &&
                            !viewData->quarantined.load()) {
                            viewsToRecover.push_back(viewData);
                        }
                    }
                }

                for (const auto& viewData : viewsToRecover) {
                    if (viewData->isDestroying.load(std::memory_order_acquire)) {
                        continue;
                    }

                    const int attempts = viewData->recoveryAttempts.fetch_add(1);
                    if (attempts >= 3) {
                        const std::uint32_t lifetimeFaults = viewData->faultCount.fetch_add(1) + 1;
                        viewData->needsRecovery = false;
                        viewData->recoveryAttempts = 0;
                        logger::error(
                            "UI Thread: View [{}] url='{}' recovery failed after {} attempts (lifetime faults={})",
                            viewData->id, viewData->originalUrl, attempts, lifetimeFaults);

                        if (lifetimeFaults >= 3 && !viewData->quarantined.exchange(true)) {
                            viewData->isHidden = true;
                            if (viewData->ultralightView) {
                                viewData->ultralightView->set_load_listener(nullptr);
                                viewData->ultralightView->set_view_listener(nullptr);
                                viewData->ultralightView = nullptr;
                            }
                            viewData->loadListener.reset();
                            viewData->viewListener.reset();
                            {
                                std::lock_guard bufferLock(viewData->bufferMutex);
                                viewData->pixelBuffer.clear();
                                viewData->pixelBuffer.shrink_to_fit();
                                viewData->newFrameReady = false;
                            }
                            viewData->pendingResourceRelease = true;
                            logger::critical(
                                "[Watchdog] View [{}] url='{}' QUARANTINED after {} lifetime faults - isolated from "
                                "the renderer to protect the game",
                                viewData->id, viewData->originalUrl, lifetimeFaults);
                        }
                        continue;
                    }

                    if (viewData->originalUrl.empty()) {
                        logger::warn("UI Thread: View [{}] needs recovery but has no originalUrl", viewData->id);
                        viewData->needsRecovery = false;
                        continue;
                    }

                    logger::info("UI Thread: Recovering View [{}] (attempt {}) by reloading original URL: {}",
                                 viewData->id, attempts + 1, viewData->originalUrl);
                    try {
                        if (!viewData->isDestroying.load(std::memory_order_acquire) && viewData->ultralightView) {
                            viewData->ultralightView->LoadURL(String(viewData->originalUrl.c_str()));
                            viewData->needsRecovery = false;
                            viewData->isLoadingFinished = false;
                        }
                    } catch (...) {
                        logger::error("UI Thread: Failed to initiate recovery for View [{}]", viewData->id);
                    }
                }

                std::vector<std::shared_ptr<PrismaView>> viewsToInitialize;
                {
                    std::shared_lock lock(viewsMutex);
                    for (const auto& pair : views) {
                        const auto& viewData = pair.second;
                        if (viewData && !viewData->isDestroying.load(std::memory_order_acquire) &&
                            !viewData->ultralightView && !viewData->htmlPathToLoad.empty() &&
                            !viewData->quarantined.load()) {
                            viewsToInitialize.push_back(viewData);
                        }
                    }
                }

                for (const auto& viewData : viewsToInitialize) {
                    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) ||
                        viewData->ultralightView) {
                        continue;
                    }

                    logger::info("UI Thread: Creating View [{}] for path: {}", viewData->id, viewData->htmlPathToLoad);
                    if (screenSize.width == 0 || screenSize.height == 0) {
                        logger::error("UI Thread: Cannot create View [{}], screen size is zero.", viewData->id);
                        continue;
                    }

                    if (!localRenderer) {
                        logger::warn("UI Thread: Renderer became null during view creation.");
                        break;
                    }

                    ViewConfig viewConfig;
                    viewConfig.is_accelerated = false;
                    viewConfig.is_transparent = true;

                    RefPtr<View> newView =
                        localRenderer->CreateView(screenSize.width, screenSize.height, viewConfig, nullptr);
                    if (viewData->isDestroying.load(std::memory_order_acquire)) {
                        newView = nullptr;
                        continue;
                    }

                    if (!newView) {
                        logger::error("UI Thread: Failed to create Ultralight View for ID [{}].", viewData->id);
                        viewData->htmlPathToLoad = "[CREATION FAILED]";
                        continue;
                    }

                    viewData->ultralightView = newView;
                    viewData->loadListener = std::make_unique<MyLoadListener>(viewData->id);
                    viewData->viewListener = std::make_unique<MyViewListener>(viewData->id);
                    viewData->ultralightView->set_load_listener(viewData->loadListener.get());
                    viewData->ultralightView->set_view_listener(viewData->viewListener.get());

                    if (viewData->isDestroying.load(std::memory_order_acquire)) {
                        viewData->ultralightView->set_load_listener(nullptr);
                        viewData->ultralightView->set_view_listener(nullptr);
                        viewData->loadListener.reset();
                        viewData->viewListener.reset();
                        viewData->ultralightView = nullptr;
                        continue;
                    }

                    viewData->ultralightView->LoadURL(String(viewData->htmlPathToLoad.c_str()));
                    viewData->ultralightView->Unfocus();
                    viewData->htmlPathToLoad.clear();
                    logger::info("UI Thread: View [{}] successfully created and loading URL.", viewData->id);
                }

                ProcessEvents();

                localRenderer->Update();
                localRenderer->Render();
                RenderViews();
            } catch (const SEHException& exception) {
                logger::critical("UI Thread: SEH Exception in render loop: {} at address 0x{:p}", exception.details(),
                                 exception.address());
                std::shared_lock lock(viewsMutex);
                for (const auto& pair : views) {
                    if (pair.second && !pair.second->isDestroying.load(std::memory_order_acquire) &&
                        !pair.second->quarantined.load()) {
                        pair.second->needsRecovery = true;
                        logger::warn("View [{}] marked for recovery after SEH exception", pair.first);
                    }
                }
            } catch (const std::exception& e) {
                logger::critical("UI Thread: Exception in render loop: {}", e.what());
                std::shared_lock lock(viewsMutex);
                for (const auto& pair : views) {
                    if (pair.second && !pair.second->isDestroying.load(std::memory_order_acquire) &&
                        !pair.second->quarantined.load()) {
                        pair.second->needsRecovery = true;
                    }
                }
            } catch (...) {
                logger::critical("UI Thread: Unknown exception in render loop (likely Ultralight internal error)");
                std::shared_lock lock(viewsMutex);
                for (const auto& pair : views) {
                    if (pair.second && !pair.second->isDestroying.load(std::memory_order_acquire) &&
                        !pair.second->quarantined.load()) {
                        pair.second->needsRecovery = true;
                    }
                }
            }
        });

        try {
            ultralightFuture.get();
        } catch (const std::exception& e) {
            logger::error("D3DPresent: Exception from UI thread: {}", e.what());
        } catch (...) {
            logger::error("D3DPresent: Unknown exception from UI thread");
        }

        try {
            RunWatchdog();
        } catch (...) {
        }

        std::vector<std::shared_ptr<PrismaView>> viewsToUpdate;
        {
            std::shared_lock lock(viewsMutex);
            viewsToUpdate.reserve(views.size());
            for (const auto& pair : views) {
                if (pair.second && !pair.second->isDestroying.load(std::memory_order_acquire) &&
                    pair.second->ultralightView) {
                    viewsToUpdate.push_back(pair.second);
                }
            }
        }

        for (const auto& viewData : viewsToUpdate) {
            UpdateSingleTextureFromBuffer(viewData);
        }

        DrawViews();
        DrawCursor();
    }

    void OnResizeBuffers() {
        std::unique_lock lock(viewsMutex);
        for (auto& [viewId, viewData] : views) {
            if (viewData) {
                ViewRenderer::ReleaseViewTexture(viewData.get());
                Inspector::ReleaseInspectorTexture(viewData.get());
            }
        }
        logger::info("OnResizeBuffers: view textures released for swap chain resize.");
    }

    void Shutdown() {
        logger::info("Shutting down PrismaUI Core System...");

        std::vector<PrismaViewId> viewIdsToDestroy;
        {
            std::shared_lock lock(viewsMutex);
            viewIdsToDestroy.reserve(views.size());
            for (const auto& pair : views) {
                viewIdsToDestroy.push_back(pair.first);
            }
        }

        for (const auto viewId : viewIdsToDestroy) {
            try {
                ViewManager::Destroy(viewId);
            } catch (const std::exception& e) {
                logger::error("Error destroying view [{}] during shutdown: {}", viewId, e.what());
            }
        }

        cursorTexture.Reset();
        spriteBatch.reset();
        commonStates.reset();
        logger::debug("DirectXTK resources released.");

        InputHandler::Shutdown();

        d3dDevice = nullptr;
        d3dContext = nullptr;
        hWnd = nullptr;
        screenSize = {};

        {
            std::unique_lock lock(viewsMutex);
            views.clear();
        }

        if (renderer) {
            // The renderer must release on the Ultralight worker that owns it.
            ultralightThread
                .submit([rendererToRelease = std::move(renderer)]() mutable {
                    logger::info("Releasing global renderer on UI thread.");
                    rendererToRelease = nullptr;
                })
                .get();
        }

        ultralightLogger.reset();
        rendererInitFailed = false;
        coreInitialized = false;
        logger::info("PrismaUI Core System shut down complete.");
    }
}
