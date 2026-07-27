#include "PCH.h"

#include "PrismaUI/SceneDepthCapture.h"

#include <array>

namespace PrismaUI::SceneDepthCapture
{
    namespace
    {
        constexpr std::uintptr_t kCommitGraphicsStateRva =
            0x1D9B5D0;
        constexpr std::uintptr_t kCaptureCallsiteRva =
            0x1D8E84A;

        constexpr std::array<std::uint8_t, 5>
            kOriginalCall{
                0xE8, 0x81, 0xCD, 0x00, 0x00
            };
        constexpr std::array<std::uint8_t, 19>
            kCommitPrologue{
                0x88, 0x54, 0x24, 0x10,
                0x88, 0x4C, 0x24, 0x08,
                0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
                0x48, 0x83, 0xEC, 0x50
            };
        constexpr std::array<std::uint8_t, 13>
            kCallsiteBoundary{
                0x33, 0xD2, 0x33, 0xC9,
                0xE8, 0x81, 0xCD, 0x00, 0x00,
                0x80, 0x7B, 0x2A, 0x00
            };

        using CommitGraphicsState =
            void (*)(std::uint8_t, std::uint8_t);

        struct Capture
        {
            Microsoft::WRL::ComPtr<IUnknown> colorIdentity;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
                originalView;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
                readOnlyView;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            D3D11_TEXTURE2D_DESC textureDescription{};
            D3D11_DEPTH_STENCIL_VIEW_DESC viewDescription{};
            D3D11_COMPARISON_FUNC comparison =
                D3D11_COMPARISON_LESS_EQUAL;
            std::uint64_t frameEpoch = 0;
        };

        std::mutex g_lifecycleMutex;
        std::mutex g_captureMutex;
        std::mutex g_targetsMutex;
        std::atomic<bool> g_installed = false;
        std::atomic<bool> g_requested = false;
        std::atomic<std::uint64_t> g_frameEpoch = 1;
        std::atomic<std::uint64_t> g_capturedEpoch = 0;
        std::atomic<std::uint32_t> g_failureLogs = 0;
        std::uintptr_t g_callsite = 0;
        CommitGraphicsState g_original = nullptr;
        Capture g_capture;
        std::array<Microsoft::WRL::ComPtr<IUnknown>, 3>
            g_submittedTargets{};

        template <std::size_t Size>
        [[nodiscard]] bool MatchesReadable(
            const void* address,
            const std::array<std::uint8_t, Size>& expected) noexcept
        {
            if (!address) {
                return false;
            }
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(
                    address,
                    &memory,
                    sizeof(memory)) != sizeof(memory) ||
                memory.State != MEM_COMMIT ||
                (memory.Protect &
                 (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                return false;
            }
            const auto start =
                reinterpret_cast<std::uintptr_t>(address);
            const auto end =
                reinterpret_cast<std::uintptr_t>(
                    memory.BaseAddress) +
                memory.RegionSize;
            return start <= end &&
                   end - start >= expected.size() &&
                   std::equal(
                       expected.begin(),
                       expected.end(),
                       static_cast<const std::uint8_t*>(address));
        }

        [[nodiscard]] Microsoft::WRL::ComPtr<IUnknown>
            Identity(IUnknown* value) noexcept
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
                   leftIdentity.Get() ==
                       rightIdentity.Get();
        }

        [[nodiscard]] bool KnownSubmittedTarget(
            IUnknown* identity) noexcept
        {
            if (!identity) {
                return false;
            }
            std::lock_guard lock(g_targetsMutex);
            return std::any_of(
                g_submittedTargets.begin(),
                g_submittedTargets.end(),
                [identity](const auto& candidate) {
                    return candidate.Get() == identity;
                });
        }

        [[nodiscard]] bool DepthExtent(
            const D3D11_TEXTURE2D_DESC& texture,
            const D3D11_DEPTH_STENCIL_VIEW_DESC& view,
            std::uint32_t& width,
            std::uint32_t& height) noexcept
        {
            std::uint32_t mip = 0;
            switch (view.ViewDimension) {
            case D3D11_DSV_DIMENSION_TEXTURE2D:
                mip = view.Texture2D.MipSlice;
                break;
            case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
                if (view.Texture2DArray.ArraySize != 1) {
                    return false;
                }
                mip = view.Texture2DArray.MipSlice;
                break;
            case D3D11_DSV_DIMENSION_TEXTURE2DMS:
                break;
            case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
                if (view.Texture2DMSArray.ArraySize != 1) {
                    return false;
                }
                break;
            default:
                return false;
            }
            if (mip >= texture.MipLevels || mip >= 32) {
                return false;
            }
            width = (std::max)(1u, texture.Width >> mip);
            height = (std::max)(1u, texture.Height >> mip);
            return true;
        }

        [[nodiscard]] bool LayoutMatches(
            const D3D11_TEXTURE2D_DESC& depthTexture,
            const D3D11_DEPTH_STENCIL_VIEW_DESC& depthView,
            const D3D11_TEXTURE2D_DESC& color) noexcept
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            return DepthExtent(
                       depthTexture,
                       depthView,
                       width,
                       height) &&
                   width == color.Width &&
                   height == color.Height &&
                   depthTexture.SampleDesc.Count ==
                       color.SampleDesc.Count &&
                   depthTexture.SampleDesc.Quality ==
                       color.SampleDesc.Quality;
        }

        [[nodiscard]] Microsoft::WRL::ComPtr<
            ID3D11DepthStencilView>
            MakeReadOnlyView(
                ID3D11Texture2D* texture,
                D3D11_DEPTH_STENCIL_VIEW_DESC description) noexcept
        {
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView> view;
            if (!texture) {
                return view;
            }
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            texture->GetDevice(device.GetAddressOf());
            if (!device) {
                return view;
            }

            description.Flags |=
                D3D11_DSV_READ_ONLY_DEPTH;
            if (description.Format ==
                    DXGI_FORMAT_D24_UNORM_S8_UINT ||
                description.Format ==
                    DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
                description.Flags |=
                    D3D11_DSV_READ_ONLY_STENCIL;
            }
            if (FAILED(device->CreateDepthStencilView(
                    texture,
                    &description,
                    view.GetAddressOf()))) {
                view.Reset();
            }
            return view;
        }

        void ReportFailure(const char* reason) noexcept
        {
            const auto index = g_failureLogs.fetch_add(
                1,
                std::memory_order_relaxed);
            if (index < 4 || index % 600 == 0) {
                logger::warn(
                    "PrismaUI scene-depth capture unavailable: {}",
                    reason);
            }
        }

        void RestoreOriginalCall() noexcept
        {
            if (g_callsite == 0) {
                return;
            }
            try {
                REL::safe_write(
                    g_callsite,
                    kOriginalCall.data(),
                    kOriginalCall.size());
            } catch (...) {
                logger::critical(
                    "PrismaUI could not roll back a failed scene-depth hook transaction");
            }
        }

        void CaptureBoundDepth() noexcept
        {
            if (!g_requested.load(std::memory_order_acquire)) {
                return;
            }
            const auto epoch = g_frameEpoch.load(
                std::memory_order_acquire);
            if (g_capturedEpoch.load(
                    std::memory_order_acquire) == epoch) {
                return;
            }

            const auto rendererData =
                RE::BSGraphics::RendererData::GetSingleton();
            const auto context = rendererData ?
                reinterpret_cast<ID3D11DeviceContext*>(
                    rendererData->context) :
                nullptr;
            if (!context) {
                return;
            }

            Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
                colorView;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
                depthView;
            context->OMGetRenderTargets(
                1,
                colorView.GetAddressOf(),
                depthView.GetAddressOf());
            if (!colorView || !depthView) {
                return;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource>
                colorResource;
            colorView->GetResource(
                colorResource.GetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D>
                colorTexture;
            if (!colorResource ||
                FAILED(colorResource.As(&colorTexture)) ||
                !colorTexture) {
                return;
            }
            const auto colorIdentity =
                Identity(colorTexture.Get());
            if (!KnownSubmittedTarget(colorIdentity.Get())) {
                return;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource>
                depthResource;
            depthView->GetResource(
                depthResource.GetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D>
                depthTexture;
            if (!depthResource ||
                FAILED(depthResource.As(&depthTexture)) ||
                !depthTexture) {
                return;
            }

            D3D11_TEXTURE2D_DESC colorDescription{};
            D3D11_TEXTURE2D_DESC depthDescription{};
            D3D11_DEPTH_STENCIL_VIEW_DESC viewDescription{};
            colorTexture->GetDesc(&colorDescription);
            depthTexture->GetDesc(&depthDescription);
            depthView->GetDesc(&viewDescription);
            if (!LayoutMatches(
                    depthDescription,
                    viewDescription,
                    colorDescription)) {
                return;
            }

            Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                depthState;
            UINT stencilReference = 0;
            context->OMGetDepthStencilState(
                depthState.GetAddressOf(),
                &stencilReference);
            if (!depthState) {
                return;
            }
            D3D11_DEPTH_STENCIL_DESC stateDescription{};
            depthState->GetDesc(&stateDescription);
            const auto comparison = stateDescription.DepthFunc;
            const auto usableComparison =
                comparison == D3D11_COMPARISON_LESS ||
                comparison == D3D11_COMPARISON_LESS_EQUAL ||
                comparison == D3D11_COMPARISON_GREATER ||
                comparison == D3D11_COMPARISON_GREATER_EQUAL;
            if (stateDescription.DepthEnable == FALSE ||
                !usableComparison) {
                return;
            }

            Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
                readOnly;
            {
                std::lock_guard lock(g_captureMutex);
                if (g_capture.originalView.Get() ==
                    depthView.Get()) {
                    readOnly = g_capture.readOnlyView;
                }
            }
            if (!readOnly) {
                readOnly = MakeReadOnlyView(
                    depthTexture.Get(),
                    viewDescription);
            }
            if (!readOnly) {
                ReportFailure(
                    "the matching DSV cannot be made read-only");
                return;
            }

            {
                std::lock_guard lock(g_captureMutex);
                if (!g_requested.load(
                        std::memory_order_acquire) ||
                    g_frameEpoch.load(
                        std::memory_order_acquire) != epoch) {
                    return;
                }
                g_capture.colorIdentity = colorIdentity;
                g_capture.originalView = depthView;
                g_capture.readOnlyView = readOnly;
                g_capture.texture = depthTexture;
                g_capture.textureDescription =
                    depthDescription;
                g_capture.viewDescription =
                    viewDescription;
                g_capture.comparison = comparison;
                g_capture.frameEpoch = epoch;
            }
            g_capturedEpoch.store(
                epoch,
                std::memory_order_release);
        }

        __declspec(noinline) void HookCommit(
            std::uint8_t firstMode,
            std::uint8_t secondMode) noexcept
        {
            if (g_original) {
                g_original(firstMode, secondMode);
            }
            CaptureBoundDepth();
        }
    }

    bool Install() noexcept
    {
        std::lock_guard lock(g_lifecycleMutex);
        if (g_installed.load(std::memory_order_acquire)) {
            return true;
        }
        if (!REL::Module::IsVR() ||
            REL::Module::get().version() !=
                F4SE::RUNTIME_VR_1_2_72) {
            return false;
        }

        bool patched = false;
        try {
            const REL::Relocation<std::uintptr_t> callsite{
                REL::Offset(kCaptureCallsiteRva)
            };
            const REL::Relocation<std::uintptr_t> target{
                REL::Offset(kCommitGraphicsStateRva)
            };
            g_callsite = callsite.address();
            if (!MatchesReadable(
                    reinterpret_cast<const void*>(
                        target.address()),
                    kCommitPrologue) ||
                g_callsite < 4 ||
                !MatchesReadable(
                    reinterpret_cast<const void*>(
                        g_callsite - 4),
                    kCallsiteBoundary)) {
                logger::error(
                    "PrismaUI scene-depth callsite failed its FO4VR 1.2.72 byte guard");
                g_callsite = 0;
                return false;
            }

            g_original = reinterpret_cast<
                CommitGraphicsState>(target.address());
            const auto original =
                F4SE::GetTrampoline().write_call<5>(
                    g_callsite,
                    HookCommit);
            patched = true;
            if (original != target.address()) {
                RestoreOriginalCall();
                patched = false;
                g_callsite = 0;
                g_original = nullptr;
                return false;
            }

            g_installed.store(true, std::memory_order_release);
            logger::info(
                "PrismaUI scene-depth capture installed for FO4VR 1.2.72");
            return true;
        } catch (...) {
            if (patched && g_callsite != 0) {
                RestoreOriginalCall();
            }
            g_callsite = 0;
            g_original = nullptr;
            return false;
        }
    }

    bool IsInstalled() noexcept
    {
        return g_installed.load(std::memory_order_acquire);
    }

    void Uninstall() noexcept
    {
        // F4SE plugins are process-lifetime modules. Rewriting a hot callsite
        // during shutdown would be less safe than leaving the guarded wrapper
        // resident; disabling capture makes it a single atomic branch.
        Reset();
    }

    void SetCaptureRequested(bool requested) noexcept
    {
        const auto previous = g_requested.exchange(
            requested,
            std::memory_order_acq_rel);
        if (previous && !requested) {
            g_capturedEpoch.store(
                0,
                std::memory_order_release);
            std::lock_guard lock(g_captureMutex);
            g_capture = {};
        }
    }

    void SetSubmittedTarget(
        ID3D11Texture2D* texture) noexcept
    {
        auto identity = Identity(texture);
        if (!identity) {
            return;
        }
        std::lock_guard lock(g_targetsMutex);
        std::size_t existing = g_submittedTargets.size();
        for (std::size_t index = 0;
             index < g_submittedTargets.size();
             ++index) {
            if (g_submittedTargets[index].Get() ==
                identity.Get()) {
                existing = index;
                break;
            }
        }
        if (existing == 0) {
            return;
        }
        const auto last = (std::min)(
            existing,
            g_submittedTargets.size() - 1);
        for (std::size_t index = last;
             index > 0;
             --index) {
            g_submittedTargets[index] =
                g_submittedTargets[index - 1];
        }
        g_submittedTargets[0] = std::move(identity);
    }

    FrameDepth AcquireForSubmittedTarget(
        ID3D11Texture2D* colorTexture,
        const D3D11_TEXTURE2D_DESC& colorDescription) noexcept
    {
        FrameDepth result;
        if (!g_requested.load(std::memory_order_acquire) ||
            !colorTexture) {
            return result;
        }

        Capture snapshot;
        {
            std::lock_guard lock(g_captureMutex);
            snapshot = g_capture;
        }
        if (!snapshot.colorIdentity ||
            !snapshot.readOnlyView ||
            !snapshot.texture ||
            snapshot.frameEpoch !=
                g_frameEpoch.load(
                    std::memory_order_acquire)) {
            return result;
        }

        const auto colorIdentity = Identity(colorTexture);
        if (!colorIdentity ||
            colorIdentity.Get() !=
                snapshot.colorIdentity.Get() ||
            !LayoutMatches(
                snapshot.textureDescription,
                snapshot.viewDescription,
                colorDescription)) {
            return result;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> colorDevice;
        Microsoft::WRL::ComPtr<ID3D11Device> depthDevice;
        colorTexture->GetDevice(colorDevice.GetAddressOf());
        snapshot.texture->GetDevice(
            depthDevice.GetAddressOf());
        if (!SameIdentity(
                colorDevice.Get(),
                depthDevice.Get())) {
            return result;
        }

        result.view = snapshot.readOnlyView;
        result.comparison = snapshot.comparison;
        result.frameEpoch = snapshot.frameEpoch;
        return result;
    }

    void AdvanceSubmittedFrame() noexcept
    {
        g_frameEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    void Reset() noexcept
    {
        g_requested.store(false, std::memory_order_release);
        g_frameEpoch.fetch_add(1, std::memory_order_acq_rel);
        g_capturedEpoch.store(0, std::memory_order_release);
        {
            std::lock_guard lock(g_targetsMutex);
            g_submittedTargets = {};
        }
        std::lock_guard lock(g_captureMutex);
        g_capture = {};
    }
}
