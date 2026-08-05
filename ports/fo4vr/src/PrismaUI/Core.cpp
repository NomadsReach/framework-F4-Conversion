#include "PCH.h"

#include "PrismaUI/Core.h"

#include "PrismaUI/D3D11GpuDriver.h"
#include "PrismaUI/D3D11StateGuard.h"
#include "PrismaUI/InputHandler.h"
#include "PrismaUI/Listeners.h"
#include "PrismaUI/SceneDepthCapture.h"
#include "PrismaUI/SpatialPointer.h"
#include "PrismaUI/SpatialPresentation.h"
#include "PrismaUI/ViewManager.h"
#include "PrismaUI/ViewRenderer.h"
#include "Utils/DllLoader.h"

#include <openvr.h>

namespace PrismaUI::Core
{
    namespace
    {
        constexpr std::uint32_t kMinimumViewDimension = 1;
        constexpr std::uint32_t kMaximumCreationAttempts = 4;
        constexpr std::uint32_t kMaximumWorkerFrameFailures = 3;
        constexpr float kDefaultHmdRefreshRate = 90.0f;
        constexpr float kMinimumHmdRefreshRate = 30.0f;
        constexpr float kMaximumHmdRefreshRate = 120.0f;

        std::atomic<bool> g_inputHookQueued = false;
        std::atomic<std::uint32_t> g_workerDelayLogs = 0;

        [[nodiscard]] bool IsExactSupportedRuntime() noexcept
        {
            return REL::Module::IsVR() &&
                   REL::Module::get().version() ==
                       F4SE::RUNTIME_VR_1_2_72;
        }

        [[nodiscard]] bool CheckedPixelCount(
            std::uint32_t width,
            std::uint32_t height,
            std::uint64_t& result) noexcept
        {
            if (width < kMinimumViewDimension ||
                height < kMinimumViewDimension ||
                width > kMaximumAcceleratedDimension ||
                height > kMaximumAcceleratedDimension) {
                return false;
            }
            result =
                static_cast<std::uint64_t>(width) *
                static_cast<std::uint64_t>(height);
            return result <= kMaximumAcceleratedPixels;
        }

        [[nodiscard]] std::optional<std::string> ToUtf8(
            const std::filesystem::path& path) noexcept
        {
            try {
                const auto& wide = path.native();
                if (wide.empty() || wide.size() > 32767) {
                    return std::nullopt;
                }
                const auto required = WideCharToMultiByte(
                    CP_UTF8,
                    WC_ERR_INVALID_CHARS,
                    wide.data(),
                    static_cast<int>(wide.size()),
                    nullptr,
                    0,
                    nullptr,
                    nullptr);
                if (required <= 0) {
                    return std::nullopt;
                }
                std::string result(
                    static_cast<std::size_t>(required),
                    '\0');
                if (WideCharToMultiByte(
                        CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        wide.data(),
                        static_cast<int>(wide.size()),
                        result.data(),
                        required,
                        nullptr,
                        nullptr) != required) {
                    return std::nullopt;
                }
                return result;
            } catch (...) {
                return std::nullopt;
            }
        }

        template <class Interface>
        [[nodiscard]] Microsoft::WRL::ComPtr<IUnknown> Identity(
            Interface* value) noexcept
        {
            Microsoft::WRL::ComPtr<IUnknown> result;
            if (value) {
                (void)value->QueryInterface(
                    IID_PPV_ARGS(result.GetAddressOf()));
            }
            return result;
        }

        [[nodiscard]] bool SameIdentity(
            IUnknown* left,
            IUnknown* right) noexcept
        {
            const auto leftIdentity = Identity(left);
            const auto rightIdentity = Identity(right);
            return leftIdentity &&
                   rightIdentity &&
                   leftIdentity.Get() == rightIdentity.Get();
        }

        [[nodiscard]] float QueryHmdRefreshRate() noexcept
        {
            try {
                auto* system = vr::VRSystem();
                if (!system) {
                    return kDefaultHmdRefreshRate;
                }
                auto error = vr::TrackedProp_Success;
                const auto refresh =
                    system->GetFloatTrackedDeviceProperty(
                        vr::k_unTrackedDeviceIndex_Hmd,
                        vr::Prop_DisplayFrequency_Float,
                        &error);
                if (error != vr::TrackedProp_Success ||
                    !std::isfinite(refresh) ||
                    refresh < kMinimumHmdRefreshRate) {
                    return kDefaultHmdRefreshRate;
                }
                return std::clamp(
                    refresh,
                    kMinimumHmdRefreshRate,
                    kMaximumHmdRefreshRate);
            } catch (...) {
                return kDefaultHmdRefreshRate;
            }
        }

        [[nodiscard]] bool CreateCursorTexture(
            Runtime& runtime) noexcept
        {
            if (runtime.cursorTexture) {
                return true;
            }
            if (!runtime.device) {
                return false;
            }

            constexpr std::uint32_t width = 16;
            constexpr std::uint32_t height = 24;
            constexpr std::uint32_t transparent = 0x00000000u;
            constexpr std::uint32_t black = 0xFF000000u;
            constexpr std::uint32_t white = 0xFFFFFFFFu;
            std::array<std::uint32_t, width * height> pixels{};
            pixels.fill(transparent);

            for (std::uint32_t y = 11; y < height; ++y) {
                for (std::uint32_t x = 3; x <= 7; ++x) {
                    const auto border =
                        x == 3 || x == 7 || y == height - 1;
                    pixels[y * width + x] =
                        border ? black : white;
                }
            }
            for (std::uint32_t y = 0; y <= 17; ++y) {
                const auto right =
                    (std::min)(y / 2u, width - 1);
                for (std::uint32_t x = 0; x <= right; ++x) {
                    const auto border =
                        x == 0 || x == right || y == 17;
                    pixels[y * width + x] =
                        border ? black : white;
                }
            }

            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial{};
            initial.pSysMem = pixels.data();
            initial.SysMemPitch = width * sizeof(std::uint32_t);

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(runtime.device->CreateTexture2D(
                    &description,
                    &initial,
                    texture.GetAddressOf())) ||
                !texture ||
                FAILED(runtime.device->CreateShaderResourceView(
                    texture.Get(),
                    nullptr,
                    runtime.cursorTexture.GetAddressOf()))) {
                runtime.cursorTexture.Reset();
                return false;
            }
            return true;
        }

        [[nodiscard]] DXGI_FORMAT RenderTargetFormat(
            DXGI_FORMAT format) noexcept
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_TYPELESS:
                return DXGI_FORMAT_B8G8R8X8_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default:
                return format;
            }
        }

        [[nodiscard]] bool EnsureSubmittedRenderTarget(
            Runtime& runtime,
            ID3D11Texture2D* texture,
            const D3D11_TEXTURE2D_DESC& description) noexcept
        {
            if (!texture ||
                !runtime.device ||
                description.Width == 0 ||
                description.Height == 0 ||
                description.ArraySize != 1 ||
                description.MipLevels == 0 ||
                description.SampleDesc.Count == 0 ||
                (description.BindFlags &
                 D3D11_BIND_RENDER_TARGET) == 0) {
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11Device> textureDevice;
            texture->GetDevice(textureDevice.GetAddressOf());
            if (!textureDevice ||
                !SameIdentity(
                    textureDevice.Get(),
                    runtime.deviceReference.Get())) {
                logger::error(
                    "PrismaUI rejected a submitted texture from a different D3D device");
                return false;
            }

            if (runtime.submittedTexture.Get() == texture &&
                runtime.submittedRenderTarget) {
                return true;
            }

            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
            auto result = runtime.device->CreateRenderTargetView(
                texture,
                nullptr,
                target.GetAddressOf());
            if (FAILED(result)) {
                D3D11_RENDER_TARGET_VIEW_DESC view{};
                view.Format =
                    RenderTargetFormat(description.Format);
                view.ViewDimension =
                    description.SampleDesc.Count > 1 ?
                        D3D11_RTV_DIMENSION_TEXTURE2DMS :
                        D3D11_RTV_DIMENSION_TEXTURE2D;
                view.Texture2D.MipSlice = 0;
                result = runtime.device->CreateRenderTargetView(
                    texture,
                    &view,
                    target.ReleaseAndGetAddressOf());
            }
            if (FAILED(result) || !target) {
                logger::error(
                    "PrismaUI could not create a view of the submitted VR texture");
                return false;
            }

            runtime.submittedTexture = texture;
            runtime.submittedRenderTarget = std::move(target);
            runtime.submittedTextureDescription = description;
            return true;
        }

        [[nodiscard]] bool BoundsToRectangle(
            const SubmittedTextureBounds& bounds,
            const D3D11_TEXTURE2D_DESC& description,
            RECT& result) noexcept
        {
            if (!bounds.valid ||
                !std::isfinite(bounds.uMin) ||
                !std::isfinite(bounds.vMin) ||
                !std::isfinite(bounds.uMax) ||
                !std::isfinite(bounds.vMax) ||
                bounds.uMin < 0.0f ||
                bounds.vMin < 0.0f ||
                bounds.uMax > 1.0f ||
                bounds.vMax > 1.0f ||
                bounds.uMax <= bounds.uMin ||
                bounds.vMax <= bounds.vMin) {
                return false;
            }

            result = {
                static_cast<LONG>(std::lround(
                    bounds.uMin * description.Width)),
                static_cast<LONG>(std::lround(
                    bounds.vMin * description.Height)),
                static_cast<LONG>(std::lround(
                    bounds.uMax * description.Width)),
                static_cast<LONG>(std::lround(
                    bounds.vMax * description.Height))
            };
            return result.left >= 0 &&
                   result.top >= 0 &&
                   result.right <=
                       static_cast<LONG>(description.Width) &&
                   result.bottom <=
                       static_cast<LONG>(description.Height) &&
                   result.right > result.left &&
                   result.bottom > result.top;
        }

        [[nodiscard]] bool UpdateStereoLayout(
            Runtime& runtime,
            const SubmittedTextureLayout& submitted,
            const D3D11_TEXTURE2D_DESC& description) noexcept
        {
            StereoPanelLayout layout;
            if (!submitted.stereoPairVerified ||
                !BoundsToRectangle(
                    submitted.left,
                    description,
                    layout.leftEyeRect) ||
                !BoundsToRectangle(
                    submitted.right,
                    description,
                    layout.rightEyeRect)) {
                runtime.stereoPanelLayout = {};
                runtime.stereoBoundsVerified.store(
                    false,
                    std::memory_order_release);
                return false;
            }

            const auto leftWidth =
                layout.leftEyeRect.right -
                layout.leftEyeRect.left;
            const auto rightWidth =
                layout.rightEyeRect.right -
                layout.rightEyeRect.left;
            const auto leftHeight =
                layout.leftEyeRect.bottom -
                layout.leftEyeRect.top;
            const auto rightHeight =
                layout.rightEyeRect.bottom -
                layout.rightEyeRect.top;
            const auto coversCanonicalAtlas =
                layout.leftEyeRect.left == 0 &&
                layout.leftEyeRect.top == 0 &&
                layout.leftEyeRect.bottom ==
                    static_cast<LONG>(description.Height) &&
                layout.rightEyeRect.top == 0 &&
                layout.rightEyeRect.bottom ==
                    static_cast<LONG>(description.Height) &&
                layout.leftEyeRect.right ==
                    layout.rightEyeRect.left &&
                layout.rightEyeRect.right ==
                    static_cast<LONG>(description.Width);
            if (leftWidth <= 0 ||
                leftHeight <= 0 ||
                leftWidth != rightWidth ||
                leftHeight != rightHeight ||
                !coversCanonicalAtlas) {
                runtime.stereoPanelLayout = {};
                runtime.stereoBoundsVerified.store(
                    false,
                    std::memory_order_release);
                return false;
            }

            layout.leftRect = layout.leftEyeRect;
            layout.rightRect = layout.rightEyeRect;
            layout.viewWidth =
                static_cast<std::uint32_t>(leftWidth);
            layout.viewHeight =
                static_cast<std::uint32_t>(leftHeight);
            layout.atlasWidth = description.Width;
            layout.atlasHeight = description.Height;
            layout.boundsVerified = true;
            layout.valid = true;
            runtime.stereoPanelLayout = layout;
            runtime.screenSize = {
                layout.viewWidth,
                layout.viewHeight
            };
            runtime.stereoBoundsVerified.store(
                true,
                std::memory_order_release);
            InputHandler::SetLogicalViewportSize(
                layout.viewWidth,
                layout.viewHeight);
            return true;
        }

        void EnsureInputHook(Runtime& runtime) noexcept
        {
            if (!runtime.gameWindow) {
                const auto rendererData =
                    RE::BSGraphics::RendererData::GetSingleton();
                const auto window = rendererData ?
                    reinterpret_cast<HWND>(
                        rendererData->renderWindow[0].hwnd) :
                    nullptr;
                if (window && IsWindow(window)) {
                    runtime.gameWindow = window;
                    InputHandler::Initialize(window);
                }
            }
            if (!runtime.gameWindow ||
                g_inputHookQueued.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                return;
            }

            const auto tasks = F4SE::GetTaskInterface();
            if (!tasks) {
                g_inputHookQueued.store(
                    false,
                    std::memory_order_release);
                return;
            }
            try {
                tasks->AddTask([] {
                    if (!InputHandler::InstallWindowHook()) {
                        g_inputHookQueued.store(
                            false,
                            std::memory_order_release);
                    }
                });
            } catch (...) {
                g_inputHookQueued.store(
                    false,
                    std::memory_order_release);
            }
        }

        struct ResizeReservation
        {
            std::uint64_t previous = 0;
            std::uint64_t held = 0;
            std::uint64_t requested = 0;
            bool inspector = false;
            bool valid = false;
        };

        [[nodiscard]] std::uint64_t& ReservationFor(
            PrismaView& view,
            bool inspector) noexcept
        {
            return inspector ?
                view.reservedInspectorPixels :
                view.reservedMainPixels;
        }

        [[nodiscard]] bool BeginResizeReservation(
            const std::shared_ptr<PrismaView>& view,
            bool inspector,
            std::uint32_t width,
            std::uint32_t height,
            ResizeReservation& reservation) noexcept
        {
            if (!view ||
                !CheckedPixelCount(
                    width,
                    height,
                    reservation.requested)) {
                return false;
            }

            auto& runtime = GetRuntime();
            std::lock_guard lock(runtime.pixelBudgetMutex);
            auto& current = ReservationFor(*view, inspector);
            if (runtime.reservedAcceleratedPixels < current) {
                logger::critical(
                    "PrismaUI accelerated pixel accounting invariant failed");
                return false;
            }
            reservation.previous = current;
            reservation.held =
                (std::max)(current, reservation.requested);
            reservation.inspector = inspector;
            const auto base =
                runtime.reservedAcceleratedPixels - current;
            if (reservation.held >
                kMaximumAcceleratedPixels - base) {
                return false;
            }
            runtime.reservedAcceleratedPixels =
                base + reservation.held;
            current = reservation.held;
            reservation.valid = true;
            return true;
        }

        void FinishResizeReservation(
            const std::shared_ptr<PrismaView>& view,
            ResizeReservation& reservation,
            bool commit) noexcept
        {
            if (!view || !reservation.valid) {
                return;
            }
            auto& runtime = GetRuntime();
            std::lock_guard lock(runtime.pixelBudgetMutex);
            auto& current =
                ReservationFor(*view, reservation.inspector);
            const auto finalValue =
                commit ?
                    reservation.requested :
                    reservation.previous;
            if (runtime.reservedAcceleratedPixels < current) {
                logger::critical(
                    "PrismaUI accelerated pixel accounting rollback failed");
                reservation.valid = false;
                return;
            }
            runtime.reservedAcceleratedPixels =
                runtime.reservedAcceleratedPixels -
                current +
                finalValue;
            current = finalValue;
            reservation.valid = false;
        }

        [[nodiscard]] bool ResizeMainView(
            const std::shared_ptr<PrismaView>& view,
            std::uint32_t width,
            std::uint32_t height) noexcept
        {
            auto& runtime = GetRuntime();
            if (!runtime.worker.IsWorkerThread() ||
                !view ||
                !view->ultralightView) {
                return false;
            }

            ResizeReservation reservation;
            if (!BeginResizeReservation(
                    view,
                    false,
                    width,
                    height,
                    reservation)) {
                return false;
            }
            try {
                view->ultralightView->Resize(width, height);
                view->viewWidth.store(
                    width,
                    std::memory_order_release);
                view->viewHeight.store(
                    height,
                    std::memory_order_release);
                FinishResizeReservation(
                    view,
                    reservation,
                    true);
                return true;
            } catch (...) {
                FinishResizeReservation(
                    view,
                    reservation,
                    false);
                return false;
            }
        }

        void RollBackViewCreation(
            const std::shared_ptr<PrismaView>& view) noexcept
        {
            if (!view) {
                return;
            }
            try {
                if (view->ultralightView) {
                    view->ultralightView->
                        set_load_listener(nullptr);
                    view->ultralightView->
                        set_view_listener(nullptr);
                    view->ultralightView->
                        set_network_listener(nullptr);
                }
            } catch (...) {
            }
            view->ultralightView = nullptr;
            view->loadListener.reset();
            view->viewListener.reset();
            view->networkListener.reset();
            view->viewWidth.store(0, std::memory_order_release);
            view->viewHeight.store(0, std::memory_order_release);
            view->loadingFinished.store(
                false,
                std::memory_order_release);
            ReleaseViewPixelReservations(view);
        }

        void CreateOrResizeViews(
            std::uint32_t fallbackWidth,
            std::uint32_t fallbackHeight) noexcept
        {
            auto& runtime = GetRuntime();
            if (!runtime.worker.IsWorkerThread() ||
                !runtime.renderer ||
                fallbackWidth == 0 ||
                fallbackHeight == 0) {
                return;
            }

            std::vector<std::shared_ptr<PrismaView>> snapshot;
            {
                std::shared_lock lock(runtime.viewsMutex);
                snapshot.reserve(runtime.views.size());
                for (const auto& [id, view] : runtime.views) {
                    (void)id;
                    if (view &&
                        !view->destroying.load(
                            std::memory_order_acquire)) {
                        snapshot.push_back(view);
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            constexpr std::array retryDelays{
                std::chrono::milliseconds(100),
                std::chrono::milliseconds(500),
                std::chrono::milliseconds(2000)
            };
            for (const auto& view : snapshot) {
                if (!view ||
                    view->destroying.load(
                        std::memory_order_acquire)) {
                    continue;
                }
                const auto [desiredWidth, desiredHeight] =
                    SpatialPresentation::GetDesiredPixelSize(
                        view,
                        fallbackWidth,
                        fallbackHeight);

                if (view->ultralightView) {
                    const auto currentWidth =
                        view->viewWidth.load(
                            std::memory_order_acquire);
                    const auto currentHeight =
                        view->viewHeight.load(
                            std::memory_order_acquire);
                    if ((currentWidth != desiredWidth ||
                         currentHeight != desiredHeight) &&
                        !ResizeMainView(
                            view,
                            desiredWidth,
                            desiredHeight)) {
                        if (view->rejectedResizeWidth !=
                                desiredWidth ||
                            view->rejectedResizeHeight !=
                                desiredHeight) {
                            logger::warn(
                                "View [{}] resize to {}x{} was rejected",
                                view->id,
                                desiredWidth,
                                desiredHeight);
                            view->rejectedResizeWidth =
                                desiredWidth;
                            view->rejectedResizeHeight =
                                desiredHeight;
                        }
                    } else {
                        view->rejectedResizeWidth = 0;
                        view->rejectedResizeHeight = 0;
                    }
                    continue;
                }

                if (view->htmlPathToLoad.empty() ||
                    view->creationAttempts >=
                        kMaximumCreationAttempts ||
                    (view->nextCreationAttempt !=
                         std::chrono::steady_clock::time_point{} &&
                     now < view->nextCreationAttempt)) {
                    continue;
                }
                if (!TryReserveMainViewPixels(
                        view,
                        desiredWidth,
                        desiredHeight)) {
                    if (view->creationBudgetWaitCount !=
                        (std::numeric_limits<std::uint32_t>::max)()) {
                        ++view->creationBudgetWaitCount;
                    }
                    const auto count =
                        view->creationBudgetWaitCount;
                    if (count == 1 || count % 600 == 0) {
                        logger::warn(
                            "View [{}] is waiting for accelerated texture capacity",
                            view->id);
                    }
                    continue;
                }

                auto created = false;
                try {
                    ultralight::ViewConfig config;
                    config.is_accelerated = true;
                    config.is_transparent = true;
                    config.initial_focus = false;
                    view->ultralightView =
                        runtime.renderer->CreateView(
                            desiredWidth,
                            desiredHeight,
                            config,
                            nullptr);
                    if (view->ultralightView) {
                        view->loadListener =
                            std::make_unique<
                                Listeners::LoadListener>(
                                view->id);
                        view->viewListener =
                            std::make_unique<
                                Listeners::ViewListener>(
                                view->id);
                        view->networkListener =
                            std::make_unique<
                                Listeners::NetworkListener>(
                                view->id,
                                &view->networkAccessPolicy);
                        view->ultralightView->
                            set_load_listener(
                                view->loadListener.get());
                        view->ultralightView->
                            set_view_listener(
                                view->viewListener.get());
                        view->ultralightView->
                            set_network_listener(
                                view->networkListener.get());
                        view->ultralightView->LoadURL(
                            ultralight::String(
                                view->htmlPathToLoad.c_str()));
                        view->ultralightView->Unfocus();
                        created = true;
                    }
                } catch (...) {
                    created = false;
                }

                if (created) {
                    view->viewWidth.store(
                        desiredWidth,
                        std::memory_order_release);
                    view->viewHeight.store(
                        desiredHeight,
                        std::memory_order_release);
                    view->htmlPathToLoad.clear();
                    view->creationAttempts = 0;
                    view->creationBudgetWaitCount = 0;
                    view->nextCreationAttempt = {};
                    logger::info(
                        "View [{}] created at {}x{}",
                        view->id,
                        desiredWidth,
                        desiredHeight);
                    continue;
                }

                RollBackViewCreation(view);
                const auto failedAttempt =
                    view->creationAttempts++;
                if (failedAttempt < retryDelays.size()) {
                    view->nextCreationAttempt =
                        now + retryDelays[failedAttempt];
                    logger::error(
                        "View [{}] creation failed; retry {}/{} scheduled",
                        view->id,
                        failedAttempt + 1,
                        kMaximumCreationAttempts);
                } else {
                    view->htmlPathToLoad.clear();
                    view->nextCreationAttempt = {};
                    logger::critical(
                        "View [{}] creation failed after {} attempts",
                        view->id,
                        kMaximumCreationAttempts);
                }
            }
        }

        [[nodiscard]] bool RunWorkerFrame(
            std::uint32_t width,
            std::uint32_t height) noexcept
        {
            auto& runtime = GetRuntime();
            if (!runtime.worker.IsWorkerThread() ||
                runtime.shuttingDown.load(
                    std::memory_order_acquire) ||
                !runtime.renderer ||
                !runtime.gpuDriver ||
                runtime.gpuDriver->HasFatalError()) {
                return false;
            }
            try {
                CreateOrResizeViews(width, height);
                InputHandler::ProcessEvents();
                runtime.renderer->Update();
                runtime.renderer->RefreshDisplay(0);
                runtime.renderer->Render();
                ViewRenderer::PublishRenderTargets();
                return !runtime.gpuDriver->HasFatalError();
            } catch (...) {
                logger::error(
                    "PrismaUI worker frame failed");
                return false;
            }
        }

        [[nodiscard]] bool PollRendererInitialization(
            Runtime& runtime) noexcept
        {
            if (runtime.rendererInitFailed.load(
                    std::memory_order_acquire)) {
                return false;
            }
            if (runtime.rendererReady.load(
                    std::memory_order_acquire)) {
                return true;
            }
            if (!runtime.rendererInitialization.valid() ||
                runtime.rendererInitialization.wait_for(
                    std::chrono::milliseconds(0)) !=
                    std::future_status::ready) {
                return false;
            }
            try {
                if (!runtime.rendererInitialization.get()) {
                    runtime.rendererInitFailed.store(
                        true,
                        std::memory_order_release);
                    return false;
                }
                return runtime.rendererReady.load(
                    std::memory_order_acquire);
            } catch (...) {
                runtime.rendererInitFailed.store(
                    true,
                    std::memory_order_release);
                logger::critical(
                    "PrismaUI Ultralight initialization task failed");
                return false;
            }
        }

        [[nodiscard]] bool WorkerFrameIsReady(
            const Runtime& runtime) noexcept
        {
            return runtime.workerFrame.valid() &&
                   runtime.workerFrame.wait_for(
                       std::chrono::milliseconds(0)) ==
                       std::future_status::ready;
        }

        [[nodiscard]] bool ConsumeWorkerFrame(
            Runtime& runtime) noexcept
        {
            if (!WorkerFrameIsReady(runtime)) {
                return true;
            }

            auto workerSuccess = false;
            try {
                workerSuccess = runtime.workerFrame.get();
            } catch (...) {
                workerSuccess = false;
            }
            const auto gpuSuccess =
                runtime.gpuDriver &&
                runtime.gpuDriver->ExecutePending();
            if (workerSuccess && gpuSuccess) {
                runtime.consecutiveWorkerFrameFailures = 0;
                ViewRenderer::CommitPublishedRenderTargets();
                return true;
            }

            ++runtime.consecutiveWorkerFrameFailures;
            logger::error(
                "PrismaUI discarded a failed worker/GPU frame ({}/{})",
                runtime.consecutiveWorkerFrameFailures,
                kMaximumWorkerFrameFailures);
            if (runtime.consecutiveWorkerFrameFailures >=
                kMaximumWorkerFrameFailures) {
                runtime.rendererInitFailed.store(
                    true,
                    std::memory_order_release);
                SpatialPointer::HandleBackendUnavailable();
            }
            return false;
        }

        void ScheduleWorkerFrame(Runtime& runtime) noexcept
        {
            if (runtime.workerFrame.valid() ||
                runtime.worker.QueuedTaskCount() != 0 ||
                !runtime.rendererReady.load(
                    std::memory_order_acquire) ||
                runtime.rendererInitFailed.load(
                    std::memory_order_acquire) ||
                runtime.shuttingDown.load(
                    std::memory_order_acquire)) {
                return;
            }

            const auto width = runtime.screenSize.width;
            const auto height = runtime.screenSize.height;
            if (width == 0 || height == 0) {
                return;
            }
            try {
                runtime.workerFrame =
                    runtime.worker.submit_with_priority(
                        SingleThreadExecutor::Priority::
                            FRAME_CRITICAL,
                        [width, height] {
                            return RunWorkerFrame(
                                width,
                                height);
                        });
            } catch (...) {
                logger::error(
                    "PrismaUI could not schedule a worker frame");
            }
        }

        class ScopedPresentationState final
        {
        public:
            explicit ScopedPresentationState(
                Runtime& runtime) noexcept :
                runtime_(runtime)
            {
                if (runtime_.context1 &&
                    runtime_.isolatedContextState) {
                    runtime_.context1->SwapDeviceContextState(
                        runtime_.isolatedContextState.Get(),
                        previous_.GetAddressOf());
                    swapped_ = previous_ != nullptr;
                }
                if (!swapped_ && runtime_.context) {
                    fallback_.emplace(runtime_.context);
                }
            }

            ~ScopedPresentationState() noexcept
            {
                fallback_.reset();
                if (!swapped_ ||
                    !runtime_.context1 ||
                    !previous_) {
                    return;
                }
                Microsoft::WRL::ComPtr<
                    ID3DDeviceContextState>
                    uiState;
                runtime_.context1->SwapDeviceContextState(
                    previous_.Get(),
                    uiState.GetAddressOf());
                if (uiState) {
                    runtime_.isolatedContextState =
                        std::move(uiState);
                }
            }

            ScopedPresentationState(
                const ScopedPresentationState&) = delete;
            ScopedPresentationState& operator=(
                const ScopedPresentationState&) = delete;

        private:
            Runtime& runtime_;
            Microsoft::WRL::ComPtr<ID3DDeviceContextState>
                previous_;
            std::optional<ScopedD3D11State> fallback_;
            bool swapped_ = false;
        };
    }

    Runtime::Runtime() = default;
    Runtime::~Runtime() = default;
    PrismaView::PrismaView(PrismaViewId viewId) noexcept :
        id(viewId)
    {}
    PrismaView::~PrismaView() = default;

    Runtime& GetRuntime() noexcept
    {
        // The process-lifetime container prevents static destruction from
        // releasing D3D or Ultralight objects after their owning runtimes.
        static auto* runtime = new Runtime();
        return *runtime;
    }

    std::shared_ptr<PrismaView> FindView(
        PrismaViewId viewId) noexcept
    {
        if (viewId == 0) {
            return nullptr;
        }
        try {
            auto& runtime = GetRuntime();
            std::shared_lock lock(runtime.viewsMutex);
            const auto iterator = runtime.views.find(viewId);
            if (iterator == runtime.views.end() ||
                !iterator->second ||
                iterator->second->destroying.load(
                    std::memory_order_acquire)) {
                return nullptr;
            }
            return iterator->second;
        } catch (...) {
            return nullptr;
        }
    }

    bool IsShuttingDown() noexcept
    {
        return GetRuntime().shuttingDown.load(
            std::memory_order_acquire);
    }

    bool IsRenderBackendOperational() noexcept
    {
        auto& runtime = GetRuntime();
        return runtime.initialized.load(
                   std::memory_order_acquire) &&
               runtime.rendererReady.load(
                   std::memory_order_acquire) &&
               !runtime.rendererInitFailed.load(
                   std::memory_order_acquire) &&
               !runtime.shuttingDown.load(
                   std::memory_order_acquire) &&
               runtime.gpuDriver &&
               !runtime.gpuDriver->HasFatalError() &&
               runtime.device &&
               runtime.context;
    }

    bool StartUltralightWorker() noexcept
    {
        auto& runtime = GetRuntime();
        if (runtime.shuttingDown.load(
                std::memory_order_acquire)) {
            return false;
        }
        if (runtime.worker.IsStarted()) {
            return true;
        }
        if (!runtime.worker.Start()) {
            logger::critical(
                "PrismaUI could not start its Ultralight worker");
            return false;
        }
        return true;
    }

    bool InitializeCoreSystem(
        ID3D11Texture2D* submittedTexture) noexcept
    {
        if (!submittedTexture ||
            !IsExactSupportedRuntime()) {
            return false;
        }

        auto& runtime = GetRuntime();
        std::lock_guard lock(runtime.presentationMutex);
        if (runtime.shuttingDown.load(
                std::memory_order_acquire)) {
            return false;
        }
        if (runtime.initialized.load(
                std::memory_order_acquire)) {
            (void)runtime.presentationThread.Observe(
                std::this_thread::get_id());
            return true;
        }
        if (!Utils::DllLoader::GetSingleton().
                LoadUltralightLibraries() ||
            !StartUltralightWorker()) {
            runtime.rendererInitFailed.store(
                true,
                std::memory_order_release);
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        submittedTexture->GetDevice(device.GetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        if (device) {
            device->GetImmediateContext(
                context.GetAddressOf());
        }
        if (!device || !context) {
            logger::critical(
                "PrismaUI could not acquire the FO4VR D3D device");
            runtime.rendererInitFailed.store(
                true,
                std::memory_order_release);
            return false;
        }

        const auto frameworkPath = Utils::GetFrameworkPath();
        const auto frameworkUtf8 =
            frameworkPath ? ToUtf8(*frameworkPath) :
                            std::nullopt;
        if (!frameworkUtf8) {
            logger::critical(
                "PrismaUI could not resolve its portable asset root");
            runtime.rendererInitFailed.store(
                true,
                std::memory_order_release);
            return false;
        }

        try {
            runtime.deviceReference = device;
            runtime.contextReference = context;
            runtime.device = device.Get();
            runtime.context = context.Get();
            (void)runtime.presentationThread.Observe(
                std::this_thread::get_id());
            runtime.commonStates =
                std::make_unique<DirectX::CommonStates>(
                    runtime.device);
            runtime.gpuDriver =
                std::make_unique<D3D11GpuDriver>(
                    runtime.device,
                    runtime.context);
            if (!runtime.gpuDriver ||
                !runtime.gpuDriver->AttachDevice(
                    runtime.device,
                    runtime.context)) {
                throw std::runtime_error(
                    "GPU driver rejected the D3D device");
            }
            if (!CreateCursorTexture(runtime)) {
                logger::warn(
                    "PrismaUI cursor texture is unavailable");
            }

            (void)runtime.deviceReference.As(
                &runtime.device1);
            (void)runtime.contextReference.As(
                &runtime.context1);
            if (runtime.device1 && runtime.context1) {
                constexpr std::array featureLevels{
                    D3D_FEATURE_LEVEL_11_1,
                    D3D_FEATURE_LEVEL_11_0,
                    D3D_FEATURE_LEVEL_10_1,
                    D3D_FEATURE_LEVEL_10_0
                };
                D3D_FEATURE_LEVEL selected{};
                const auto stateResult =
                    runtime.device1->
                        CreateDeviceContextState(
                            0,
                            featureLevels.data(),
                            static_cast<UINT>(
                                featureLevels.size()),
                            D3D11_SDK_VERSION,
                            __uuidof(ID3D11Device),
                            &selected,
                            runtime.isolatedContextState.
                                GetAddressOf());
                if (FAILED(stateResult)) {
                    runtime.isolatedContextState.Reset();
                    logger::warn(
                        "PrismaUI will use bounded D3D state restoration");
                }
            }
        } catch (...) {
            runtime.gpuDriver.reset();
            runtime.commonStates.reset();
            runtime.device = nullptr;
            runtime.context = nullptr;
            runtime.deviceReference.Reset();
            runtime.contextReference.Reset();
            runtime.rendererInitFailed.store(
                true,
                std::memory_order_release);
            logger::critical(
                "PrismaUI D3D initialization failed");
            return false;
        }

        EnsureInputHook(runtime);
        const auto refreshRate = QueryHmdRefreshRate();
        try {
            runtime.rendererInitialization =
                runtime.worker.submit_with_priority(
                    SingleThreadExecutor::Priority::FRAME_CRITICAL,
                    [assetRoot = *frameworkUtf8,
                     refreshRate]() noexcept {
                        auto& state = GetRuntime();
                        try {
                            auto& platform =
                                ultralight::Platform::instance();
                            state.ultralightLogger =
                                std::make_unique<
                                    Listeners::UltralightLogger>();
                            platform.set_logger(
                                state.ultralightLogger.get());
                            platform.set_font_loader(
                                ultralight::
                                    GetPlatformFontLoader());
                            platform.set_file_system(
                                ultralight::
                                    GetPlatformFileSystem(
                                        ultralight::String(
                                            assetRoot.c_str())));
                            platform.set_gpu_driver(
                                state.gpuDriver.get());

                            ultralight::Config config;
                            config.resource_path_prefix =
                                "resources/";
                            config.override_ram_size =
                                1024u * 1024u * 1024u;
                            config.min_large_heap_size =
                                8u * 1024u * 1024u;
                            config.min_small_heap_size =
                                512u * 1024u;
                            config.memory_cache_size =
                                32u * 1024u * 1024u;
                            config.page_cache_size = 0;
                            config.num_renderer_threads = 2;
                            config.animation_timer_delay =
                                1.0 /
                                static_cast<double>(refreshRate);
                            config.scroll_timer_delay =
                                config.animation_timer_delay;
                            platform.set_config(config);

                            state.renderer =
                                ultralight::Renderer::Create();
                            if (!state.renderer) {
                                state.rendererInitFailed.store(
                                    true,
                                    std::memory_order_release);
                                return false;
                            }
                            state.rendererReady.store(
                                true,
                                std::memory_order_release);
                            logger::info(
                                "PrismaUI Ultralight GPU renderer initialized at {:.1f} Hz",
                                refreshRate);
                            return true;
                        } catch (...) {
                            state.rendererInitFailed.store(
                                true,
                                std::memory_order_release);
                            logger::critical(
                                "PrismaUI Ultralight renderer initialization failed");
                            return false;
                        }
                    });
        } catch (...) {
            runtime.rendererInitFailed.store(
                true,
                std::memory_order_release);
            logger::critical(
                "PrismaUI could not queue Ultralight renderer initialization");
            return false;
        }

        runtime.initialized.store(
            true,
            std::memory_order_release);
        logger::info(
            "PrismaUI FO4VR graphics initialization accepted");
        return true;
    }

    void RenderSubmittedTexture(
        ID3D11Texture2D* texture,
        const SubmittedTextureLayout& layout) noexcept
    {
        if (!texture ||
            !IsExactSupportedRuntime()) {
            return;
        }
        auto& runtime = GetRuntime();
        if (!runtime.initialized.load(
                std::memory_order_acquire) &&
            !InitializeCoreSystem(texture)) {
            return;
        }

        std::lock_guard lock(runtime.presentationMutex);
        if (runtime.shuttingDown.load(
                std::memory_order_acquire) ||
            runtime.rendererInitFailed.load(
                std::memory_order_acquire)) {
            return;
        }
        const auto threadObservation =
            runtime.presentationThread.Observe(
                std::this_thread::get_id());
        if (threadObservation.migrated &&
            (threadObservation.migrationCount <= 4 ||
             threadObservation.migrationCount % 600 == 0)) {
            logger::info(
                "PrismaUI serialized presentation moved to another FO4VR engine thread (migration {})",
                threadObservation.migrationCount);
        }

        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (!EnsureSubmittedRenderTarget(
                runtime,
                texture,
                description) ||
            !UpdateStereoLayout(
                runtime,
                layout,
                description)) {
            SpatialPointer::HandleBackendUnavailable();
            return;
        }
        SceneDepthCapture::SetSubmittedTarget(texture);
        EnsureInputHook(runtime);

        if (!PollRendererInitialization(runtime)) {
            return;
        }

        const auto workerWasDelayed =
            runtime.workerFrame.valid() &&
            !WorkerFrameIsReady(runtime);
        if (workerWasDelayed) {
            const auto count =
                g_workerDelayLogs.fetch_add(
                    1,
                    std::memory_order_relaxed);
            if (count < 3 || count % 1200 == 0) {
                logger::debug(
                    "PrismaUI reused the prior completed UI frame");
            }
        }

        SceneDepthCapture::FrameDepth depth =
            SceneDepthCapture::
                AcquireForSubmittedTarget(
                    texture,
                    description);
        {
            ScopedPresentationState state(runtime);
            (void)ConsumeWorkerFrame(runtime);
            if (!runtime.rendererInitFailed.load(
                    std::memory_order_acquire)) {
                auto* target =
                    runtime.submittedRenderTarget.Get();
                runtime.context->OMSetRenderTargets(
                    1,
                    &target,
                    depth.IsValid() ?
                        depth.view.Get() :
                        nullptr);
                ViewRenderer::DrawViews(
                    ViewRenderer::RenderLayout::
                        HeadLockedStereo,
                    depth.IsValid() ? &depth : nullptr);
                ViewRenderer::DrawCursor(
                    ViewRenderer::RenderLayout::
                        HeadLockedStereo);
            }
        }

        runtime.renderEpoch.fetch_add(
            1,
            std::memory_order_release);
        ScheduleWorkerFrame(runtime);
    }

    void OnResizeBuffers() noexcept
    {
        auto& runtime = GetRuntime();
        std::lock_guard lock(runtime.presentationMutex);
        runtime.submittedRenderTarget.Reset();
        runtime.submittedTexture.Reset();
        runtime.submittedTextureDescription = {};
        runtime.stereoPanelLayout = {};
        runtime.screenSize = {};
        runtime.stereoBoundsVerified.store(
            false,
            std::memory_order_release);
        InputHandler::SetLogicalViewportSize(0, 0);
        SceneDepthCapture::Reset();
    }

    void Shutdown() noexcept
    {
        auto& runtime = GetRuntime();
        if (runtime.shuttingDown.exchange(
                true,
                std::memory_order_acq_rel)) {
            return;
        }
        logger::info("PrismaUI shutdown began");

        ViewManager::ReleaseAllFocus();
        SpatialPointer::Shutdown();
        InputHandler::Shutdown();

        std::vector<std::shared_ptr<PrismaView>> snapshot;
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

        if (runtime.worker.IsStarted()) {
            (void)runtime.worker.TryPost(
                SingleThreadExecutor::Priority::FRAME_CRITICAL,
                [snapshot] {
                    for (const auto& view : snapshot) {
                        if (!view) {
                            continue;
                        }
                        try {
                            if (view->ultralightView) {
                                view->ultralightView->
                                    set_load_listener(nullptr);
                                view->ultralightView->
                                    set_view_listener(nullptr);
                                view->ultralightView->
                                    set_network_listener(nullptr);
                            }
                        } catch (...) {
                        }
                        view->inspectorView = nullptr;
                        view->ultralightView = nullptr;
                        view->loadListener.reset();
                        view->viewListener.reset();
                        view->networkListener.reset();
                        ReleaseViewPixelReservations(view);
                    }
                    auto& state = GetRuntime();
                    state.renderer = nullptr;
                    try {
                        auto& platform =
                            ultralight::Platform::instance();
                        platform.set_gpu_driver(nullptr);
                        platform.set_logger(nullptr);
                    } catch (...) {
                    }
                    state.rendererReady.store(
                        false,
                        std::memory_order_release);
                });
            (void)runtime.worker.StopAndJoin(true);
        }

        {
            std::unique_lock lock(runtime.viewsMutex);
            runtime.views.clear();
        }
        {
            std::lock_guard lock(runtime.jsCallbacksMutex);
            runtime.jsCallbacks.clear();
        }
        {
            std::lock_guard lock(runtime.presentationMutex);
            SceneDepthCapture::Reset();
            ViewRenderer::ReleaseDeviceResources();
            runtime.submittedRenderTarget.Reset();
            runtime.submittedTexture.Reset();
            runtime.cursorTexture.Reset();
            runtime.commonStates.reset();
            runtime.gpuDriver.reset();
            runtime.isolatedContextState.Reset();
            runtime.context1.Reset();
            runtime.device1.Reset();
            runtime.contextReference.Reset();
            runtime.deviceReference.Reset();
            runtime.context = nullptr;
            runtime.device = nullptr;
            runtime.ultralightLogger.reset();
            runtime.stereoPanelLayout = {};
            runtime.screenSize = {};
        }
        runtime.initialized.store(
            false,
            std::memory_order_release);
        logger::info("PrismaUI shutdown completed");
    }

    bool TryReserveMainViewPixels(
        const std::shared_ptr<PrismaView>& view,
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        if (!view) {
            return false;
        }
        std::uint64_t requested = 0;
        if (!CheckedPixelCount(
                width,
                height,
                requested)) {
            return false;
        }

        auto& runtime = GetRuntime();
        std::lock_guard lock(runtime.pixelBudgetMutex);
        const auto current = view->reservedMainPixels;
        if (runtime.reservedAcceleratedPixels < current) {
            return false;
        }
        const auto base =
            runtime.reservedAcceleratedPixels - current;
        if (requested >
            kMaximumAcceleratedPixels - base) {
            return false;
        }
        runtime.reservedAcceleratedPixels =
            base + requested;
        view->reservedMainPixels = requested;
        return true;
    }

    bool TryReserveInspectorPixels(
        const std::shared_ptr<PrismaView>& view,
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        if (!view) {
            return false;
        }
        std::uint64_t requested = 0;
        if (!CheckedPixelCount(
                width,
                height,
                requested)) {
            return false;
        }

        auto& runtime = GetRuntime();
        std::lock_guard lock(runtime.pixelBudgetMutex);
        const auto current =
            view->reservedInspectorPixels;
        if (runtime.reservedAcceleratedPixels < current) {
            return false;
        }
        const auto base =
            runtime.reservedAcceleratedPixels - current;
        if (requested >
            kMaximumAcceleratedPixels - base) {
            return false;
        }
        runtime.reservedAcceleratedPixels =
            base + requested;
        view->reservedInspectorPixels = requested;
        return true;
    }

    void ReleaseViewPixelReservations(
        const std::shared_ptr<PrismaView>& view) noexcept
    {
        if (!view) {
            return;
        }
        auto& runtime = GetRuntime();
        std::lock_guard lock(runtime.pixelBudgetMutex);
        const auto release =
            view->reservedMainPixels +
            view->reservedInspectorPixels;
        if (release > runtime.reservedAcceleratedPixels) {
            logger::critical(
                "View [{}] accelerated pixel release invariant failed",
                view->id);
            runtime.reservedAcceleratedPixels = 0;
        } else {
            runtime.reservedAcceleratedPixels -= release;
        }
        view->reservedMainPixels = 0;
        view->reservedInspectorPixels = 0;
    }

    void ReleaseInspectorPixelReservation(
        const std::shared_ptr<PrismaView>& view) noexcept
    {
        if (!view) {
            return;
        }
        auto& runtime = GetRuntime();
        std::lock_guard lock(runtime.pixelBudgetMutex);
        const auto release =
            view->reservedInspectorPixels;
        if (release > runtime.reservedAcceleratedPixels) {
            logger::critical(
                "View [{}] inspector pixel release invariant failed",
                view->id);
            runtime.reservedAcceleratedPixels = 0;
        } else {
            runtime.reservedAcceleratedPixels -= release;
        }
        view->reservedInspectorPixels = 0;
    }

    bool ResizeInspectorView(
        const std::shared_ptr<PrismaView>& view,
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        auto& runtime = GetRuntime();
        if (!runtime.worker.IsWorkerThread() ||
            !view ||
            !view->inspectorView) {
            return false;
        }

        ResizeReservation reservation;
        if (!BeginResizeReservation(
                view,
                true,
                width,
                height,
                reservation)) {
            return false;
        }
        try {
            view->inspectorView->Resize(width, height);
            FinishResizeReservation(
                view,
                reservation,
                true);
            return true;
        } catch (...) {
            FinishResizeReservation(
                view,
                reservation,
                false);
            return false;
        }
    }
}
