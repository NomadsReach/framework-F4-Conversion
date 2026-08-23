#include "Core.h"

#include <eh.h>
#include <psapi.h>

#include "Communication.h"
#include "InputHandler.h"
#include "Inspector.h"
#include "Listeners.h"
#include "RenderRetirement.h"
#include "Utils/DllLoader.h"
#include "ViewManager.h"
#include "ViewOperationQueue.h"
#include "ViewRenderer.h"

namespace {
class SEHException : public std::exception {
public:
    SEHException(unsigned int code, EXCEPTION_POINTERS* ep)
        : code_(code), address_(nullptr), accessType_(0), accessAddress_(0)
    {
        if (ep && ep->ExceptionRecord) {
            address_ = ep->ExceptionRecord->ExceptionAddress;
            if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
                accessType_ = ep->ExceptionRecord->ExceptionInformation[0];
                accessAddress_ = ep->ExceptionRecord->ExceptionInformation[1];
            }
        }
    }

    const char* what() const noexcept override { return "Windows Structured Exception"; }
    unsigned int code() const { return code_; }
    void* address() const { return address_; }

    std::string details() const
    {
        std::string msg;
        switch (code_) {
            case EXCEPTION_ACCESS_VIOLATION: {
                msg = "Access Violation";
                const char* op = accessType_ == 0 ? "read" : "write";
                char buf[128];
                snprintf(buf, sizeof(buf), " (%s at 0x%p)", op, reinterpret_cast<void*>(accessAddress_));
                msg += buf;
                break;
            }
            case EXCEPTION_STACK_OVERFLOW:
                msg = "Stack Overflow";
                break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                msg = "Integer Divide by Zero";
                break;
            default: {
                char buf[64];
                snprintf(buf, sizeof(buf), "Code 0x%08X", code_);
                msg = buf;
                break;
            }
        }
        return msg;
    }

private:
    unsigned int code_;
    void* address_;
    ULONG_PTR accessType_;
    ULONG_PTR accessAddress_;
};

void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* ep)
{
    if (code == EXCEPTION_STACK_OVERFLOW) return;
    throw SEHException(code, ep);
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

PrismaView::~PrismaView() = default;

void InitializeCoreSystem()
{
    logger::info("Initializing PrismaUI Core System...");
    InitHooks();

    const auto basePath = Utils::GetBasePath();
    ultralightThread.submit([basePath]() {
        try {
            Platform& plat = Platform::instance();
            ultralightLogger = std::make_unique<MyUltralightLogger>();
            plat.set_logger(ultralightLogger.get());
            plat.set_font_loader(ultralight::GetPlatformFontLoader());
            plat.set_file_system(ultralight::GetPlatformFileSystem(basePath.string().c_str()));

            Config config;
            config.resource_path_prefix = "resources/";
            config.override_ram_size = 1024u * 1024u * 1024u;
            config.min_large_heap_size = 8u * 1024u * 1024u;
            config.min_small_heap_size = 512u * 1024u;
            config.memory_cache_size = 32u * 1024u * 1024u;
            config.page_cache_size = 0;
            config.num_renderer_threads = 2;
            plat.set_config(config);

            renderer = Renderer::Create();
            if (!renderer) {
                logger::critical("Failed to create Ultralight Renderer!");
                rendererInitFailed = true;
            } else {
                logger::info("Ultralight Renderer created.");
            }
        } catch (const std::exception& e) {
            logger::critical("Exception during Ultralight initialization: {}", e.what());
            rendererInitFailed = true;
        } catch (...) {
            logger::critical("Unknown exception during Ultralight initialization.");
            rendererInitFailed = true;
        }
    }).get();

    if (auto* task = F4SE::GetTaskInterface()) {
        task->AddUITask([] {
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->RegisterMenu(FocusMenu::MENU_NAME.data(), FocusMenu::Creator);
            }
        });
    }

    logger::info("PrismaUI Core System Initialized.");
}

void InitHooks()
{
    logger::debug("D3D hooks are installed from the game-data-ready handler.");
}

void InitGraphics()
{
    auto* rendererData = RE::BSGraphics::GetRendererData();
    if (!rendererData) {
        logger::critical("InitGraphics: BSGraphics::RendererData is null!");
        return;
    }

    if (!d3dDevice) d3dDevice = reinterpret_cast<ID3D11Device*>(rendererData->device);
    if (!d3dContext) d3dContext = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

    if (!hWnd && rendererData->renderWindow[0].hwnd) {
        hWnd = reinterpret_cast<HWND>(rendererData->renderWindow[0].hwnd);
        screenSize.width = static_cast<std::uint32_t>(rendererData->renderWindow[0].windowWidth);
        screenSize.height = static_cast<std::uint32_t>(rendererData->renderWindow[0].windowHeight);

        static std::atomic<bool> inputHandlerInitialized = false;
        bool expected = false;
        if (inputHandlerInitialized.compare_exchange_strong(expected, true)) {
            Initialize(hWnd, &ultralightThread, &views, &viewsMutex);
            if (auto* task = F4SE::GetTaskInterface()) {
                task->AddTask([] {
                    if (!InstallWndProcHook()) {
                        logger::error("Failed to install PrismaUI WndProc subclass.");
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
                spriteBatch = std::make_unique<DirectX::SpriteBatch>(d3dContext);
                logger::info("DirectXTK resources initialized.");
            } catch (const std::exception& e) {
                logger::critical("Failed to initialize DirectXTK: {}", e.what());
                commonStates.reset();
                spriteBatch.reset();
            }
        }

        if (!cursorTexture) {
            auto cursorPath = Utils::GetBasePath() / "misc" / "cursor.png";
            const HRESULT hr = DirectX::CreateWICTextureFromFile(
                d3dDevice, cursorPath.wstring().c_str(), nullptr, &cursorTexture);
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

void RunWatchdog()
{
    constexpr auto kInterval = std::chrono::seconds(30);
    static auto lastRun = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastRun < kInterval) return;
    lastRun = now;

    std::vector<std::shared_ptr<PrismaView>> snap;
    {
        std::shared_lock lock(viewsMutex);
        snap.reserve(views.size());
        for (auto& [id, view] : views) {
            if (view && !view->isDestroying.load(std::memory_order_acquire)) snap.push_back(view);
        }
    }
    if (snap.empty()) return;

    std::size_t workingSetMB = 0;
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        workingSetMB = pmc.WorkingSetSize / (1024 * 1024);
    }

    int visible = 0;
    int hidden = 0;
    int quarantined = 0;
    int faulting = 0;
    for (const auto& view : snap) {
        if (view->quarantined.load()) ++quarantined;
        if (view->isHidden.load()) ++hidden;
        else ++visible;
        if (view->faultCount.load() > 0) ++faulting;
    }

    logger::info("[Watchdog] views={} (visible={} hidden={} quarantined={}) faulting={} workingSet={}MB",
                 snap.size(), visible, hidden, quarantined, faulting, workingSetMB);

    for (const auto& view : snap) {
        if (view->faultCount.load() > 0 || view->quarantined.load()) {
            logger::warn("[Watchdog] view[{}] url='{}' hidden={} faults={} quarantined={}", view->id,
                         view->originalUrl, view->isHidden.load(), view->faultCount.load(), view->quarantined.load());
        }
    }

    constexpr std::size_t kPressureMB = 6000;
    if (workingSetMB <= kPressureMB) return;

    int reclaimed = 0;
    for (const auto& view : snap) {
        if (!view->isHidden.load() || view->quarantined.load()) continue;
        bool freed = false;
        if (view->texture || view->textureView || view->inspectorTexture || view->inspectorTextureView) {
            view->pendingResourceRelease = true;
            freed = true;
        }
        {
            std::lock_guard lock(view->bufferMutex);
            if (!view->pixelBuffer.empty()) {
                view->pixelBuffer.clear();
                view->pixelBuffer.shrink_to_fit();
                view->bufferWidth = 0;
                view->bufferHeight = 0;
                view->bufferStride = 0;
                view->newFrameReady = false;
                freed = true;
            }
        }
        if (freed) ++reclaimed;
    }

    if (reclaimed > 0) {
        logger::warn("[Watchdog] memory pressure ({}MB > {}MB): reclaimed {} hidden view(s)", workingSetMB,
                     kPressureMB, reclaimed);
    }
}

void D3DPresent(uint32_t a_p1)
{
    (void)a_p1;
    if (!coreInitialized || rendererInitFailed) return;

    if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) {
        InitGraphics();
        if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) return;
    }

    RenderRetirement::Drain();

    std::vector<PrismaViewId> viewsWithPendingRelease;
    {
        std::shared_lock lock(viewsMutex);
        for (const auto& [id, view] : views) {
            if (view && !view->isDestroying.load(std::memory_order_acquire) && view->pendingResourceRelease.load()) {
                viewsWithPendingRelease.push_back(id);
            }
        }
    }

    for (const auto viewId : viewsWithPendingRelease) {
        std::shared_ptr<PrismaView> viewData;
        {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it != views.end()) viewData = it->second;
        }
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) continue;

        ViewRenderer::ReleaseViewTexture(viewData.get());
        Inspector::ReleaseInspectorTexture(viewData.get());
        viewData->pendingResourceRelease = false;
    }

    ViewOperationQueue::ProcessAllViewOperations();

    auto ultralightFuture = ultralightThread.submit([dev = d3dDevice, ctx = d3dContext, hwnd = hWnd]() {
        static bool sehTranslatorSet = false;
        if (!sehTranslatorSet) {
            _set_se_translator(SEHTranslator);
            sehTranslatorSet = true;
        }

        try {
            if (!dev || !ctx || !hwnd) return;

            auto localRenderer = renderer;
            if (!localRenderer) return;

            std::vector<std::shared_ptr<PrismaView>> viewsToRecover;
            {
                std::shared_lock lock(viewsMutex);
                for (const auto& [id, view] : views) {
                    if (view && !view->isDestroying.load(std::memory_order_acquire) && view->needsRecovery.load() &&
                        view->ultralightView && !view->quarantined.load()) {
                        viewsToRecover.push_back(view);
                    }
                }
            }

            for (const auto& viewData : viewsToRecover) {
                if (viewData->isDestroying.load(std::memory_order_acquire)) continue;

                const int attempts = viewData->recoveryAttempts.fetch_add(1);
                if (attempts >= 3) {
                    const std::uint32_t lifetimeFaults = viewData->faultCount.fetch_add(1) + 1;
                    viewData->needsRecovery = false;
                    viewData->recoveryAttempts = 0;
                    logger::error("UI Thread: View [{}] url='{}' recovery failed after {} attempts (faults={})",
                                  viewData->id, viewData->originalUrl, attempts, lifetimeFaults);

                    if (lifetimeFaults >= 3 && !viewData->quarantined.exchange(true)) {
                        viewData->isHidden = true;
                        if (viewData->ultralightView) {
                            viewData->ultralightView->set_load_listener(nullptr);
                            viewData->ultralightView->set_view_listener(nullptr);
                            viewData->ultralightView = nullptr;
                        }
                        {
                            std::lock_guard lock(viewData->bufferMutex);
                            viewData->pixelBuffer.clear();
                            viewData->pixelBuffer.shrink_to_fit();
                            viewData->newFrameReady = false;
                        }
                        viewData->pendingResourceRelease = true;
                        logger::critical("[Watchdog] View [{}] url='{}' quarantined after {} faults", viewData->id,
                                         viewData->originalUrl, lifetimeFaults);
                    }
                    continue;
                }

                if (viewData->originalUrl.empty()) {
                    viewData->needsRecovery = false;
                    continue;
                }

                try {
                    viewData->ultralightView->LoadURL(String(viewData->originalUrl.c_str()));
                    viewData->needsRecovery = false;
                    viewData->isLoadingFinished = false;
                } catch (...) {
                    logger::error("UI Thread: Failed to recover View [{}]", viewData->id);
                }
            }

            std::vector<std::shared_ptr<PrismaView>> viewsToInitialize;
            {
                std::shared_lock lock(viewsMutex);
                for (const auto& [id, view] : views) {
                    if (view && !view->isDestroying.load(std::memory_order_acquire) && !view->ultralightView &&
                        !view->htmlPathToLoad.empty() && !view->quarantined.load()) {
                        viewsToInitialize.push_back(view);
                    }
                }
            }

            for (const auto& viewData : viewsToInitialize) {
                if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || viewData->ultralightView) {
                    continue;
                }
                if (screenSize.width == 0 || screenSize.height == 0) continue;

                ViewConfig config;
                config.is_accelerated = false;
                config.is_transparent = true;
                viewData->ultralightView = localRenderer->CreateView(screenSize.width, screenSize.height, config, nullptr);

                if (viewData->ultralightView) {
                    viewData->loadListener = std::make_unique<Listeners::MyLoadListener>(viewData->id);
                    viewData->viewListener = std::make_unique<Listeners::MyViewListener>(viewData->id);
                    viewData->ultralightView->set_load_listener(viewData->loadListener.get());
                    viewData->ultralightView->set_view_listener(viewData->viewListener.get());
                    viewData->ultralightView->LoadURL(String(viewData->htmlPathToLoad.c_str()));
                    viewData->ultralightView->Unfocus();
                    viewData->htmlPathToLoad.clear();
                    logger::info("UI Thread: View [{}] created.", viewData->id);
                } else {
                    logger::error("UI Thread: Failed to create View [{}].", viewData->id);
                    viewData->htmlPathToLoad = "[CREATION FAILED]";
                }
            }

            ProcessEvents();
            localRenderer->Update();
            localRenderer->Render();
            RenderViews();
        } catch (const SEHException& seh) {
            logger::critical("UI Thread: SEH exception: {} at 0x{:p}", seh.details(), seh.address());
            std::shared_lock lock(viewsMutex);
            for (const auto& [id, view] : views) {
                if (view && !view->isDestroying.load(std::memory_order_acquire) && !view->quarantined.load()) {
                    view->needsRecovery = true;
                }
            }
        } catch (const std::exception& e) {
            logger::critical("UI Thread: Exception: {}", e.what());
            std::shared_lock lock(viewsMutex);
            for (const auto& [id, view] : views) {
                if (view && !view->isDestroying.load(std::memory_order_acquire) && !view->quarantined.load()) {
                    view->needsRecovery = true;
                }
            }
        } catch (...) {
            logger::critical("UI Thread: Unknown exception.");
            std::shared_lock lock(viewsMutex);
            for (const auto& [id, view] : views) {
                if (view && !view->isDestroying.load(std::memory_order_acquire) && !view->quarantined.load()) {
                    view->needsRecovery = true;
                }
            }
        }
    });

    try {
        ultralightFuture.get();
    } catch (const std::exception& e) {
        logger::error("D3DPresent: UI thread exception: {}", e.what());
    } catch (...) {
        logger::error("D3DPresent: UI thread exception.");
    }

    try {
        RunWatchdog();
    } catch (...) {
    }

    std::vector<std::shared_ptr<PrismaView>> viewsToCheck;
    {
        std::shared_lock lock(viewsMutex);
        viewsToCheck.reserve(views.size());
        for (const auto& [id, view] : views) {
            if (view && !view->isDestroying.load(std::memory_order_acquire) && view->ultralightView) {
                viewsToCheck.push_back(view);
            }
        }
    }

    for (const auto& viewData : viewsToCheck) UpdateSingleTextureFromBuffer(viewData);

    DrawViews();
    DrawCursor();
}

void OnResizeBuffers()
{
    std::unique_lock lock(viewsMutex);
    for (auto& [id, view] : views) {
        if (!view) continue;
        ViewRenderer::ReleaseViewTexture(view.get());
        Inspector::ReleaseInspectorTexture(view.get());
    }
    logger::info("OnResizeBuffers: view textures released.");
}

void Shutdown()
{
    logger::info("Shutting down PrismaUI Core System...");

    std::vector<PrismaViewId> viewIdsToDestroy;
    {
        std::shared_lock lock(viewsMutex);
        for (const auto& [id, view] : views) viewIdsToDestroy.push_back(id);
    }

    for (const auto id : viewIdsToDestroy) {
        try {
            ViewManager::Destroy(id);
        } catch (const std::exception& e) {
            logger::error("Error destroying View [{}]: {}", id, e.what());
        }
    }

    RenderRetirement::Drain();

    cursorTexture.Reset();
    spriteBatch.reset();
    commonStates.reset();

    InputHandler::Shutdown();

    d3dDevice = nullptr;
    d3dContext = nullptr;
    hWnd = nullptr;

    {
        std::unique_lock lock(viewsMutex);
        views.clear();
    }

    if (renderer) {
        ultralightThread.submit([rendererMoved = std::move(renderer)]() mutable {
            rendererMoved = nullptr;
        }).get();
    }

    ultralightLogger.reset();
    coreInitialized = false;
    logger::info("PrismaUI Core System shut down complete.");
}

}
