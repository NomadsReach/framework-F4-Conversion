#include "Core.h"

#include <eh.h>  // For _set_se_translator
#include <psapi.h>  // K32GetProcessMemoryInfo (watchdog memory monitoring)

#include "Communication.h"
#include "InputHandler.h"
#include "Inspector.h"
#include "Listeners.h"
#include "Utils/DllLoader.h"
#include "ViewManager.h"
#include "ViewOperationQueue.h"
#include "ViewRenderer.h"

namespace {
    // SEH exception class to convert structured exceptions to C++ exceptions
    // Copies all relevant data from EXCEPTION_POINTERS since that pointer is only
    // valid during the translator call
    class SEHException : public std::exception {
    public:
        SEHException(unsigned int code, EXCEPTION_POINTERS* ep)
            : code_(code), address_(nullptr), accessType_(0), accessAddress_(0) {
            if (ep && ep->ExceptionRecord) {
                address_ = ep->ExceptionRecord->ExceptionAddress;
                // For access violations, capture the operation type and target address
                if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
                    accessType_ = ep->ExceptionRecord->ExceptionInformation[0];
                    accessAddress_ = ep->ExceptionRecord->ExceptionInformation[1];
                }
            }
        }

        const char* what() const noexcept override { return "Windows Structured Exception"; }

        unsigned int code() const { return code_; }
        void* address() const { return address_; }

        std::string details() const {
            std::string msg;
            switch (code_) {
                case EXCEPTION_ACCESS_VIOLATION:
                    msg = "Access Violation";
                    {
                        const char* op = accessType_ == 0 ? "read" : "write";
                        char buf[128];
                        snprintf(buf, sizeof(buf), " (%s at 0x%p)", op, (void*)accessAddress_);
                        msg += buf;
                    }
                    break;
                case EXCEPTION_STACK_OVERFLOW:
                    msg = "Stack Overflow";
                    break;
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                    msg = "Integer Divide by Zero";
                    break;
                default:
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Code 0x%08X", code_);
                    msg = buf;
                    break;
            }
            return msg;
        }

    private:
        unsigned int code_;
        void* address_;
        ULONG_PTR accessType_;     // 0 = read, 1 = write, 8 = DEP violation
        ULONG_PTR accessAddress_;  // Address that was accessed
    };

    void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* ep) {
        // Stack overflow: cannot throw (no stack for unwinding). System will terminate gracefully.
        if (code == EXCEPTION_STACK_OVERFLOW) {
            return;
        }
        throw SEHException(code, ep);
    }
}  // namespace

namespace PrismaUI::Core {
    using namespace PrismaUI::Listeners;
    using namespace PrismaUI::ViewRenderer;
    using namespace PrismaUI::ViewManager;
    using namespace PrismaUI::InputHandler;

    SingleThreadExecutor ultralightThread;
    NanoIdGenerator generator;
    std::atomic<bool> coreInitialized = false;
    std::atomic<bool> rendererInitFailed = false;

    // Ultralight platform objects - ownership remains with caller per API docs
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

    std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> PrismaUI::Core::jsCallbacks;
    std::mutex PrismaUI::Core::jsCallbacksMutex;

    PrismaView::~PrismaView() { ViewRenderer::ReleaseViewTexture(this); }

    void InitializeCoreSystem() {
        logger::info("Initializing PrismaUI Core System...");
        InitHooks();

        const auto basePath = Utils::GetBasePath();
        ultralightThread
            .submit([basePath]() {
                try {
                    Platform& plat = Platform::instance();
                    ultralightLogger = std::make_unique<MyUltralightLogger>();
                    plat.set_logger(ultralightLogger.get());
                    plat.set_font_loader(ultralight::GetPlatformFontLoader());

                    plat.set_file_system(ultralight::GetPlatformFileSystem(basePath.string().c_str()));

                    Config config;
                    config.resource_path_prefix = "resources/";

                    // --- Memory tuning (critical inside Fallout 4) ---
                    // Ultralight/JavaScriptCore auto-detect the machine's physical RAM (often
                    // 16-32 GB) and size their caches/JS heaps generously PER VIEW. Inside FO4 we
                    // run many views alongside the engine's own (capped) Scaleform heap, so the
                    // stock config bloats process memory and starves Scaleform's allocator -> CTD
                    // (Scaleform::SysAllocMapper::allocMem failing on a memory-starved process).
                    // Tell JSC it has far less RAM and shrink the initial heaps/caches/threads.
                    // Conservative values: meaningful memory savings, negligible UI impact.
                    config.override_ram_size    = 1024u * 1024u * 1024u;  // pretend 1 GB (was: detected 16-32 GB)
                    config.min_large_heap_size  = 8u * 1024u * 1024u;     // 32 MB -> 8 MB initial JS large heap
                    config.min_small_heap_size  = 512u * 1024u;           // 1 MB  -> 512 KB initial JS small heap
                    config.memory_cache_size    = 32u * 1024u * 1024u;    // 64 MB -> 32 MB WebCore resource cache
                    config.page_cache_size      = 0;                      // no back/forward page cache (we never navigate)
                    config.num_renderer_threads = 2;                      // cap paint threads (was ~physical_cores - 1)

                    plat.set_config(config);

                    renderer = Renderer::Create();
                    if (!renderer) {
                        logger::critical("Failed to create Ultralight Renderer!");
                        rendererInitFailed = true;
                    } else {
                        logger::info(
                            "Ultralight Platform configured and Renderer created on UI "
                            "thread.");
                    }
                } catch (const std::exception& e) {
                    logger::critical(
                        "Exception during Ultralight Platform/Renderer init on UI "
                        "thread: {}",
                        e.what());
                    rendererInitFailed = true;
                } catch (...) {
                    logger::critical(
                        "Unknown exception during Ultralight Platform/Renderer init on "
                        "UI thread.");
                    rendererInitFailed = true;
                }
            })
            .get();

        auto ui = RE::UI::GetSingleton();
        ui->RegisterMenu(FocusMenu::MENU_NAME.data(), FocusMenu::Creator);

        logger::info("PrismaUI Core System Initialized.");
    }

    void InitHooks() {
        // MinHook D3D hooks are installed via Hooks::D3DHooks::Install() on kGameDataReady in main.cpp.
        logger::debug("InitHooks: D3D hooks installed via MinHook (see main.cpp kGameDataReady handler).");
    }

    void InitGraphics() {
        auto* rendererData = RE::BSGraphics::GetRendererData();
        if (!rendererData) {
            logger::critical("InitGraphics: BSGraphics::RendererData is null!");
            return;
        }

        if (!d3dDevice)  d3dDevice  = reinterpret_cast<ID3D11Device*>(rendererData->device);
        if (!d3dContext) d3dContext = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

        if (!hWnd && rendererData->renderWindow[0].hwnd) {
            hWnd = reinterpret_cast<HWND>(rendererData->renderWindow[0].hwnd);
            screenSize.width  = static_cast<std::uint32_t>(rendererData->renderWindow[0].windowWidth);
            screenSize.height = static_cast<std::uint32_t>(rendererData->renderWindow[0].windowHeight);

            static std::atomic<bool> input_handler_initialized = false;
            bool expected_ih_init = false;

            if (input_handler_initialized.compare_exchange_strong(expected_ih_init, true)) {
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

        if (d3dDevice && d3dContext) {
            if (!commonStates || !spriteBatch) {
                try {
                    commonStates = std::make_unique<DirectX::CommonStates>(d3dDevice);
                    spriteBatch  = std::make_unique<DirectX::SpriteBatch>(d3dContext);
                    logger::info("DirectXTK SpriteBatch and CommonStates initialized.");
                } catch (const std::exception& e) {
                    logger::critical("Failed to initialize DirectXTK: {}", e.what());
                    commonStates.reset();
                    spriteBatch.reset();
                }
            }

            if (!cursorTexture && d3dDevice) {
                auto cursorPath = Utils::GetBasePath() / "misc" / "cursor.png";
                HRESULT hr =
                    DirectX::CreateWICTextureFromFile(d3dDevice, cursorPath.wstring().c_str(), nullptr, &cursorTexture);
                if (SUCCEEDED(hr)) {
                    logger::info("Cursor texture loaded successfully.");
                } else {
                    logger::error("Failed to load cursor texture from '{}'. HRESULT: 0x{:08X}", cursorPath.string(),
                                  static_cast<unsigned int>(hr));
                    cursorTexture.Reset();
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Watchdog: periodic health monitor for all hosted plugin views.
    //
    // Runs on the present thread (D3D-texture ops are valid here). Every 30s it:
    //   * logs a one-line health summary (view count, visible/hidden/quarantined,
    //     faulting views, process working set) so a misbehaving plugin is visible
    //     in the log,
    //   * logs a detail line for any view with faults or that's quarantined
    //     (the originalUrl identifies which plugin/page it is), and
    //   * under process-memory pressure, frees the screen-sized render buffers
    //     (CPU pixel buffer + GPU texture) of HIDDEN views — they're regenerated
    //     when the view is shown again, so this is lossless.
    //
    // Quarantine itself (removing a repeatedly-faulting view from the renderer) is
    // done in the recovery pass on the UI thread; this just reports + reclaims.
    // -------------------------------------------------------------------------
    void RunWatchdog() {
        // Wall-clock throttle (frame count is unreliable — FPS varies 30-144).
        // 30s heartbeat: frequent enough to catch a runaway view, quiet in the log.
        // (Quarantine itself is immediate — it runs in the per-frame recovery pass,
        // not here — so a slow heartbeat doesn't delay isolation of a broken view.)
        constexpr auto kInterval = std::chrono::seconds(30);
        static auto lastRun = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - lastRun < kInterval) return;
        lastRun = now;

        std::vector<std::shared_ptr<PrismaView>> snap;
        {
            std::shared_lock lock(viewsMutex);
            snap.reserve(views.size());
            for (auto& [id, v] : views)
                if (v) snap.push_back(v);
        }
        if (snap.empty()) return;

        std::size_t workingSetMB = 0;
        PROCESS_MEMORY_COUNTERS pmc{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            workingSetMB = pmc.WorkingSetSize / (1024 * 1024);

        int visible = 0, hidden = 0, quarantined = 0, faulting = 0;
        for (auto& v : snap) {
            if (v->quarantined.load()) ++quarantined;
            if (v->isHidden.load()) ++hidden; else ++visible;
            if (v->faultCount.load() > 0) ++faulting;
        }

        logger::info("[Watchdog] views={} (visible={} hidden={} quarantined={}) faulting={} workingSet={}MB",
                     snap.size(), visible, hidden, quarantined, faulting, workingSetMB);

        for (auto& v : snap) {
            if (v->faultCount.load() > 0 || v->quarantined.load()) {
                logger::warn("[Watchdog]   view[{}] url='{}' hidden={} faults={} quarantined={}",
                             v->id, v->originalUrl, v->isHidden.load(),
                             v->faultCount.load(), v->quarantined.load());
            }
        }

        // Memory-pressure relief: reclaim render buffers of hidden views.
        // We do NOT release the D3D texture directly here — Destroy() releases the
        // same texture from the owning plugin's thread, so a direct release could
        // double-free. Instead flag pendingResourceRelease and let the present-thread
        // handler (top of D3DPresent) free it safely next frame. The CPU pixel buffer
        // is guarded by bufferMutex (Destroy clears it under the same lock), so that's
        // safe to clear here.
        constexpr std::size_t kPressureMB = 6000;
        if (workingSetMB > kPressureMB) {
            int reclaimed = 0;
            for (auto& v : snap) {
                if (!v->isHidden.load() || v->quarantined.load()) continue;
                bool freed = false;
                if (v->texture || v->textureView) { v->pendingResourceRelease = true; freed = true; }
                {
                    std::lock_guard<std::mutex> bl(v->bufferMutex);
                    if (!v->pixelBuffer.empty()) {
                        v->pixelBuffer.clear();
                        v->pixelBuffer.shrink_to_fit();
                        v->bufferWidth = v->bufferHeight = v->bufferStride = 0;
                        v->newFrameReady = false;
                        freed = true;
                    }
                }
                if (freed) ++reclaimed;
            }
            if (reclaimed > 0)
                logger::warn("[Watchdog] memory pressure ({}MB > {}MB): reclaimed render buffers for {} hidden view(s)",
                             workingSetMB, kPressureMB, reclaimed);
        }
    }

    void D3DPresent(uint32_t a_p1) {
        // Note: In F4, the MinHook trampoline calls the original Present before invoking this function.
        // RealD3dPresentFunc is not used here; see Hooks.cpp for the hook installation.
        (void)a_p1;

        if (!coreInitialized || rendererInitFailed) return;

        if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) {
            InitGraphics();
            if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) return;
        }

        std::vector<PrismaViewId> viewsWithPendingRelease;
        {
            std::shared_lock lock(viewsMutex);
            for (const auto& pair : views) {
                if (pair.second && pair.second->pendingResourceRelease.load()) {
                    viewsWithPendingRelease.push_back(pair.first);
                }
            }
        }

        for (const auto& viewId : viewsWithPendingRelease) {
            std::shared_ptr<PrismaView> viewData = nullptr;
            {
                std::shared_lock lock(viewsMutex);
                auto it = views.find(viewId);
                if (it != views.end()) {
                    viewData = it->second;
                }
            }

            if (viewData) {
                logger::debug(
                    "D3DPresent: Releasing D3D resources for View [{}] from render "
                    "thread",
                    viewId);
                ViewRenderer::ReleaseViewTexture(viewData.get());
                Inspector::ReleaseInspectorTexture(viewData.get());
                viewData->pendingResourceRelease = false;
            }
        }

        // Process pending operations for all views
        ViewOperationQueue::ProcessAllViewOperations();

        auto ultralightFuture = ultralightThread.submit([dev = d3dDevice, ctx = d3dContext, hwnd = hWnd]() {
            // Enable SEH to C++ exception translation for this thread (only needs to be
            // set once per thread)
            static bool sehTranslatorSet = false;
            if (!sehTranslatorSet) {
                _set_se_translator(SEHTranslator);
                sehTranslatorSet = true;
            }

            try {
                if (!dev || !ctx || !hwnd) {
                    logger::warn("UI Thread: D3D device/context/hwnd is null, skipping frame.");
                    return;
                }

                // Capture renderer locally to avoid race with shutdown
                auto localRenderer = renderer;
                if (!localRenderer) {
                    logger::warn("UI Thread: Renderer is null, skipping frame.");
                    return;
                }

                // Check for views that need recovery after an exception
                std::vector<std::shared_ptr<PrismaView>> viewsToRecover;
                {
                    std::shared_lock lock(viewsMutex);
                    for (auto& pair : views) {
                        if (pair.second && pair.second->needsRecovery.load() && pair.second->ultralightView &&
                            !pair.second->quarantined.load()) {
                            viewsToRecover.push_back(pair.second);
                        }
                    }
                }

                for (auto& viewData : viewsToRecover) {
                    int attempts = viewData->recoveryAttempts.fetch_add(1);
                    if (attempts >= 3) {
                        // This incident's recovery failed. Count it against the view's
                        // lifetime fault total; if it keeps faulting, quarantine it so a
                        // persistently-broken plugin view can't corrupt the renderer every
                        // frame. Reset the per-incident counter for any future incident.
                        std::uint32_t lifetimeFaults = viewData->faultCount.fetch_add(1) + 1;
                        viewData->needsRecovery = false;
                        viewData->recoveryAttempts = 0;
                        logger::error(
                            "UI Thread: View [{}] url='{}' recovery failed after {} attempts "
                            "(lifetime faults={})",
                            viewData->id, viewData->originalUrl, attempts, lifetimeFaults);

                        if (lifetimeFaults >= 3 && !viewData->quarantined.load()) {
                            viewData->quarantined = true;
                            viewData->isHidden = true;
                            // Remove from the Ultralight renderer so it stops being
                            // updated/painted (where it keeps faulting). The PrismaView
                            // handle stays valid; the owning plugin can still Destroy it.
                            if (viewData->ultralightView) {
                                viewData->ultralightView->set_load_listener(nullptr);
                                viewData->ultralightView->set_view_listener(nullptr);
                                viewData->ultralightView = nullptr;
                            }
                            {
                                std::lock_guard<std::mutex> bl(viewData->bufferMutex);
                                viewData->pixelBuffer.clear();
                                viewData->pixelBuffer.shrink_to_fit();
                                viewData->newFrameReady = false;
                            }
                            // Free the leftover GPU texture via the safe present-thread path.
                            viewData->pendingResourceRelease = true;
                            logger::critical(
                                "[Watchdog] View [{}] url='{}' QUARANTINED after {} lifetime faults "
                                "— isolated from the renderer to protect the game",
                                viewData->id, viewData->originalUrl, lifetimeFaults);
                        }
                        continue;
                    }

                    // Use original URL for recovery (the entry point that sets up
                    // everything)
                    if (!viewData->originalUrl.empty()) {
                        logger::info(
                            "UI Thread: Recovering View [{}] (attempt {}) by reloading "
                            "original URL: {}",
                            viewData->id, attempts + 1, viewData->originalUrl);
                        try {
                            viewData->ultralightView->LoadURL(String(viewData->originalUrl.c_str()));
                            viewData->needsRecovery = false;
                            viewData->isLoadingFinished = false;
                            // recoveryAttempts will be reset by OnFinishLoading on successful
                            // load
                        } catch (...) {
                            logger::error("UI Thread: Failed to initiate recovery for View [{}]", viewData->id);
                        }
                    } else {
                        logger::warn("UI Thread: View [{}] needs recovery but has no originalUrl", viewData->id);
                        viewData->needsRecovery = false;  // Clear flag to avoid infinite loop
                    }
                }

                std::vector<std::shared_ptr<PrismaView>> viewsToInitialize;
                {
                    std::shared_lock lock(viewsMutex);
                    for (auto& pair : views) {
                        if (pair.second && !pair.second->ultralightView && !pair.second->htmlPathToLoad.empty() &&
                            !pair.second->quarantined.load()) {
                            viewsToInitialize.push_back(pair.second);
                        }
                    }
                }

                for (auto& viewData : viewsToInitialize) {
                    if (!viewData || viewData->ultralightView) continue;

                    logger::info("UI Thread: Creating View [{}] for path: {}", viewData->id, viewData->htmlPathToLoad);

                    if (screenSize.width == 0 || screenSize.height == 0) {
                        logger::error("UI Thread: Cannot create View [{}], screen size is zero.", viewData->id);
                        continue;
                    }

                    // Re-check renderer before creating view
                    if (!localRenderer) {
                        logger::warn("UI Thread: Renderer became null during view creation.");
                        break;
                    }

                    ViewConfig view_config;
                    view_config.is_accelerated = false;
                    view_config.is_transparent = true;

                    viewData->ultralightView =
                        localRenderer->CreateView(screenSize.width, screenSize.height, view_config, nullptr);

                    if (viewData->ultralightView) {
                        viewData->loadListener = std::make_unique<Listeners::MyLoadListener>(viewData->id);
                        viewData->viewListener = std::make_unique<Listeners::MyViewListener>(viewData->id);
                        viewData->ultralightView->set_load_listener(viewData->loadListener.get());
                        viewData->ultralightView->set_view_listener(viewData->viewListener.get());
                        viewData->ultralightView->LoadURL(String(viewData->htmlPathToLoad.c_str()));
                        viewData->ultralightView->Unfocus();
                        viewData->htmlPathToLoad.clear();
                        logger::info("UI Thread: View [{}] successfully created and loading URL.", viewData->id);
                    } else {
                        logger::error("UI Thread: Failed to create Ultralight View for ID [{}].", viewData->id);
                        viewData->htmlPathToLoad = "[CREATION FAILED]";
                    }
                }

                ProcessEvents();

                if (localRenderer) {
                    localRenderer->Update();
                    localRenderer->Render();
                }

                RenderViews();
            } catch (const SEHException& seh) {
                logger::critical("UI Thread: SEH Exception in render loop: {} at address 0x{:p}", seh.details(),
                                 seh.address());
                // Mark all views for recovery - the renderer state is likely corrupted
                {
                    std::shared_lock lock(viewsMutex);
                    for (auto& pair : views) {
                        if (pair.second && !pair.second->quarantined.load()) {
                            pair.second->needsRecovery = true;
                            logger::warn("View [{}] marked for recovery after SEH exception", pair.first);
                        }
                    }
                }
            } catch (const std::exception& e) {
                logger::critical("UI Thread: Exception in render loop: {}", e.what());
                // Mark all views for recovery
                {
                    std::shared_lock lock(viewsMutex);
                    for (auto& pair : views) {
                        if (pair.second && !pair.second->quarantined.load()) {
                            pair.second->needsRecovery = true;
                        }
                    }
                }
            } catch (...) {
                // Unknown exceptions (likely from Ultralight/WebCore internals)
                logger::critical(
                    "UI Thread: Unknown exception in render loop (likely Ultralight "
                    "internal error)");
                // Mark all views for recovery
                {
                    std::shared_lock lock(viewsMutex);
                    for (auto& pair : views) {
                        if (pair.second && !pair.second->quarantined.load()) {
                            pair.second->needsRecovery = true;
                        }
                    }
                }
            }
        });

        // Wait for UI thread but handle any exceptions that might have escaped
        try {
            ultralightFuture.get();
        } catch (const std::exception& e) {
            logger::error("D3DPresent: Exception from UI thread: {}", e.what());
        } catch (...) {
            logger::error("D3DPresent: Unknown exception from UI thread");
        }

        // Periodic health monitor for all hosted plugin views (self-throttles to ~5s).
        try {
            RunWatchdog();
        } catch (...) {
            // The watchdog must never take the game down; swallow anything it throws.
        }

        std::vector<std::shared_ptr<PrismaView>> viewsToCheck;
        {
            std::shared_lock lock(viewsMutex);
            viewsToCheck.reserve(views.size());
            for (const auto& pair : views) {
                if (pair.second && pair.second->ultralightView) {
                    viewsToCheck.push_back(pair.second);
                }
            }
        }

        for (const auto& viewData : viewsToCheck) {
            UpdateSingleTextureFromBuffer(viewData);
        }

        DrawViews();
        DrawCursor();
    }

    void OnResizeBuffers() {
        std::unique_lock lock(viewsMutex);
        for (auto& [id, view] : views) {
            if (view) {
                ViewRenderer::ReleaseViewTexture(view.get());
            }
        }
        logger::info("OnResizeBuffers: view textures released for swap chain resize.");
    }

    void Shutdown() {
        logger::info("Shutting down PrismaUI Core System...");

        std::vector<PrismaViewId> viewIdsToDestroy;
        {
            std::shared_lock lock(viewsMutex);
            for (const auto& pair : views) {
                viewIdsToDestroy.push_back(pair.first);
            }
        }

        for (const auto& id : viewIdsToDestroy) {
            try {
                ViewManager::Destroy(id);
            } catch (const std::exception& e) {
                logger::error("Error destroying view [{}] during shutdown: {}", id, e.what());
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

        {
            std::unique_lock lock(viewsMutex);
            views.clear();
        }
        if (renderer) {
            // Move renderer to the lambda so it's the sole owner,
            // ensuring release happens on the UI thread
            ultralightThread
                .submit([renderer_moved = std::move(renderer)]() mutable {
                    logger::info("Releasing global renderer on UI thread.");
                    renderer_moved = nullptr;
                })
                .get();
        }

        // Release Ultralight platform objects after renderer is destroyed
        ultralightLogger.reset();

        coreInitialized = false;
        logger::info("PrismaUI Core System shut down complete.");
    }
}  // namespace PrismaUI::Core
