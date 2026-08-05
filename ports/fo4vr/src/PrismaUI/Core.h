#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/String.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

#include <DirectXTK/CommonStates.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include "PrismaUI/PresentationThreadPolicy.h"
#include "PrismaUI/SpatialPointerProtocol.h"
#include "PrismaUI_F4_API.h"
#include "PrismaUI_F4VR_API.h"
#include "Utils/NanoID.h"
#include "Utils/SingleThreadExecutor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

namespace PrismaUI
{
    class D3D11GpuDriver;
}

namespace PrismaUI::Listeners
{
    class LoadListener;
    class ViewListener;
    class NetworkListener;
    class UltralightLogger;
}

namespace PrismaUI::Core
{
    using PrismaViewId = std::uint64_t;
    using SimpleJSCallback = std::function<void(std::string)>;

    inline constexpr std::size_t kMaximumFrameworkViews = 64;
    inline constexpr std::uint32_t kMaximumAcceleratedDimension = 4096;
    inline constexpr std::uint64_t kMaximumAcceleratedPixels =
        24ull * 1024ull * 1024ull;

    struct SpatialRuntimeState
    {
        PRISMA_UI_VR_API::SpatialUpdateV1 pending{};
        PRISMA_UI_VR_API::SpatialUpdateV1 active{};
        PRISMA_UI_VR_API::SpatialUpdateV1 applied{};
        std::uint64_t acceptedSequence = 0;
        std::uint64_t appliedSequence = 0;
        std::uint32_t replacedPendingUpdateCount = 0;
        PRISMA_UI_VR_API::SpatialResult lastApplyResult =
            PRISMA_UI_VR_API::SpatialResult::NotReady;
        bool hasPending = false;
        bool hasActive = false;
        bool hasApplied = false;
        bool backendReady = false;
    };

    struct SpatialPointerRuntimeState
    {
        static constexpr std::size_t kSampleCapacity = 16;

        std::array<
            PRISMA_UI_VR_API::SpatialPointerUpdateV1,
            kSampleCapacity>
            samples{};
        std::size_t sampleHead = 0;
        std::size_t sampleCount = 0;
        PRISMA_UI_VR_API::SpatialPointerUpdateV1 active{};
        std::uint64_t acceptedSequence = 0;
        std::uint64_t appliedSequence = 0;
        std::uint32_t replacedPendingUpdateCount = 0;
        PRISMA_UI_VR_API::SpatialResult lastApplyResult =
            PRISMA_UI_VR_API::SpatialResult::NotReady;
        std::chrono::steady_clock::time_point lastSubmissionTime{};
        bool hasActive = false;
        bool backendReady = false;
        bool routed = false;
        bool hasHit = false;
        SpatialPointerProtocol::State interaction{};
        float hitDistance = 0.0f;
        float hitUv[2]{};
    };

    struct GpuRenderTargetSnapshot
    {
        bool valid = false;
        std::uint32_t textureId = 0;
        std::uint32_t renderBufferId = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t textureWidth = 0;
        std::uint32_t textureHeight = 0;
        ultralight::BitmapFormat format =
            ultralight::BitmapFormat::BGRA8_UNORM_SRGB;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
        std::uint64_t generation = 0;
    };

    struct InspectorPresentationState
    {
        float x = 0.0f;
        float y = 0.0f;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        float opacity = 1.0f;
    };

    struct PrismaView
    {
        explicit PrismaView(PrismaViewId viewId) noexcept;
        ~PrismaView();

        PrismaView(const PrismaView&) = delete;
        PrismaView& operator=(const PrismaView&) = delete;

        PrismaViewId id = 0;

        std::atomic<PRISMA_UI_VR_API::NetworkAccessPolicy>
            networkAccessPolicy{
                PRISMA_UI_VR_API::NetworkAccessPolicy::Unrestricted
            };

        std::unique_ptr<Listeners::LoadListener> loadListener;
        std::unique_ptr<Listeners::ViewListener> viewListener;
        std::unique_ptr<Listeners::NetworkListener> networkListener;
        ultralight::RefPtr<ultralight::View> ultralightView;
        ultralight::RefPtr<ultralight::View> inspectorView;

        std::string htmlPathToLoad;
        std::string originalHtmlPath;
        std::string originalUrl;

        std::atomic<std::uint32_t> viewWidth = 0;
        std::atomic<std::uint32_t> viewHeight = 0;
        std::atomic<bool> hidden = false;
        std::atomic<bool> loadingFinished = false;
        std::atomic<bool> destroying = false;

        std::function<void(PrismaViewId)> domReadyCallback;
        std::function<void(
            PrismaViewId,
            PRISMA_UI_API::ConsoleMessageLevel,
            const std::string&)>
            consoleMessageCallback;
        std::mutex callbackMutex;

        std::mutex translationMutex;
        std::string translationPluginName;
        std::shared_ptr<const std::string> translationScript;
        std::atomic<std::uint64_t> translationRevision = 0;
        std::atomic<std::uint64_t> injectedTranslationRevision = 0;
        std::atomic<bool> windowObjectReady = false;

        std::atomic<int> scrollingPixelSize = 28;
        std::atomic<int> order = 0;

        std::atomic<bool> focused = false;
        std::atomic<bool> focusRequestPending = false;
        std::atomic<std::uint64_t> focusRequestGeneration = 0;
        std::mutex focusRequestMutex;
        std::atomic<bool> deferredFocusPending = false;
        std::atomic<bool> deferredFocusPauseGame = false;
        std::atomic<bool> deferredFocusDisableFocusMenu = false;
        std::atomic<bool> usesFocusMenu = false;
        std::atomic<bool> paused = false;

        std::atomic<bool> inspectorVisible = false;
        std::atomic<bool> inspectorPointerHover = false;
        std::mutex inspectorPresentationMutex;
        InspectorPresentationState inspectorPresentation;

        std::uint32_t creationAttempts = 0;
        std::uint32_t creationBudgetWaitCount = 0;
        std::chrono::steady_clock::time_point nextCreationAttempt{};

        std::uint64_t reservedMainPixels = 0;
        std::uint64_t reservedInspectorPixels = 0;
        std::uint32_t rejectedResizeWidth = 0;
        std::uint32_t rejectedResizeHeight = 0;

        std::mutex spatialMutex;
        SpatialRuntimeState spatial;
        std::mutex spatialPointerMutex;
        SpatialPointerRuntimeState spatialPointer;

        std::mutex renderTargetMutex;
        GpuRenderTargetSnapshot renderTarget;
        GpuRenderTargetSnapshot inspectorRenderTarget;
        GpuRenderTargetSnapshot pendingRenderTarget;
        GpuRenderTargetSnapshot pendingInspectorRenderTarget;
    };

    struct SubmittedTextureBounds
    {
        bool valid = false;
        float uMin = 0.0f;
        float vMin = 0.0f;
        float uMax = 1.0f;
        float vMax = 1.0f;
    };

    struct SubmittedTextureLayout
    {
        SubmittedTextureBounds left;
        SubmittedTextureBounds right;
        bool stereoPairVerified = false;
    };

    struct StereoPanelLayout
    {
        bool valid = false;
        bool boundsVerified = false;
        RECT leftEyeRect{};
        RECT rightEyeRect{};
        RECT leftRect{};
        RECT rightRect{};
        std::uint32_t viewWidth = 0;
        std::uint32_t viewHeight = 0;
        std::uint32_t atlasWidth = 0;
        std::uint32_t atlasHeight = 0;
    };

    struct JSCallbackData
    {
        PrismaViewId viewId = 0;
        std::string name;
        SimpleJSCallback callback;
    };

    struct Runtime final
    {
        Runtime();
        ~Runtime();

        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        SingleThreadExecutor worker;
        NanoIdGenerator idGenerator;

        std::atomic<bool> initialized = false;
        std::atomic<bool> rendererReady = false;
        std::atomic<bool> rendererInitFailed = false;
        std::atomic<bool> shuttingDown = false;
        std::atomic<bool> stereoBoundsVerified = false;

        ultralight::RefPtr<ultralight::Renderer> renderer;
        std::unique_ptr<Listeners::UltralightLogger> ultralightLogger;
        std::unique_ptr<D3D11GpuDriver> gpuDriver;

        Microsoft::WRL::ComPtr<ID3D11Device> deviceReference;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> contextReference;
        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState>
            isolatedContextState;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        HWND gameWindow = nullptr;
        PresentationThreadPolicy::SerializedTracker<
            std::thread::id>
            presentationThread;
        std::future<bool> rendererInitialization;
        std::future<bool> workerFrame;
        std::uint32_t consecutiveWorkerFrameFailures = 0;

        struct ScreenSize
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
        } screenSize;

        StereoPanelLayout stereoPanelLayout;
        std::unique_ptr<DirectX::CommonStates> commonStates;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorTexture;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> submittedTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            submittedRenderTarget;
        D3D11_TEXTURE2D_DESC submittedTextureDescription{};

        std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
        mutable std::shared_mutex viewsMutex;

        std::map<
            std::pair<PrismaViewId, std::string>,
            JSCallbackData>
            jsCallbacks;
        std::mutex jsCallbacksMutex;

        std::mutex pixelBudgetMutex;
        std::uint64_t reservedAcceleratedPixels = 0;

        std::mutex presentationMutex;
        std::atomic<std::uint64_t> renderEpoch = 0;
    };

    [[nodiscard]] Runtime& GetRuntime() noexcept;
    [[nodiscard]] std::shared_ptr<PrismaView> FindView(
        PrismaViewId viewId) noexcept;
    [[nodiscard]] bool IsShuttingDown() noexcept;
    [[nodiscard]] bool IsRenderBackendOperational() noexcept;

    [[nodiscard]] bool StartUltralightWorker() noexcept;
    [[nodiscard]] bool InitializeCoreSystem(
        ID3D11Texture2D* submittedTexture) noexcept;
    void RenderSubmittedTexture(
        ID3D11Texture2D* texture,
        const SubmittedTextureLayout& layout) noexcept;
    void OnResizeBuffers() noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] bool TryReserveMainViewPixels(
        const std::shared_ptr<PrismaView>& view,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] bool TryReserveInspectorPixels(
        const std::shared_ptr<PrismaView>& view,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    void ReleaseViewPixelReservations(
        const std::shared_ptr<PrismaView>& view) noexcept;
    void ReleaseInspectorPixelReservation(
        const std::shared_ptr<PrismaView>& view) noexcept;
    [[nodiscard]] bool ResizeInspectorView(
        const std::shared_ptr<PrismaView>& view,
        std::uint32_t width,
        std::uint32_t height) noexcept;
}
