#include "PCH.h"

#include "Hooks/Hooks.h"

#include "Hooks/EngineStereoSubmissionPolicy.h"
#include "PrismaUI/Core.h"
#include "PrismaUI/SceneDepthCapture.h"

#include <MinHook.h>

namespace Hooks
{
    namespace
    {
        std::mutex g_installMutex;
        std::atomic<bool> g_engineInstalled = false;
        std::atomic<bool> g_resizeInstalled = false;
        using SubmitStereoTexture =
            void (*)(ID3D11Texture2D* texture);
        using ResizeBuffers =
            HRESULT(APIENTRY*)(
                IDXGISwapChain* swapChain,
                UINT bufferCount,
                UINT width,
                UINT height,
                DXGI_FORMAT newFormat,
                UINT flags);
        SubmitStereoTexture g_originalSubmit = nullptr;
        ResizeBuffers g_originalResize = nullptr;
        void* g_resizeTarget = nullptr;

        [[nodiscard]] bool IsExactSupportedRuntime() noexcept
        {
            return REL::Module::IsVR() &&
                   REL::Module::get().version() ==
                       F4SE::RUNTIME_VR_1_2_72;
        }

        [[nodiscard]] bool IsReadable(
            const void* address,
            std::size_t size) noexcept
        {
            if (!address || size == 0) {
                return false;
            }

            auto* cursor =
                static_cast<const std::byte*>(address);
            const auto* end = cursor + size;
            while (cursor < end) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(
                        cursor,
                        &memory,
                        sizeof(memory)) != sizeof(memory) ||
                    memory.State != MEM_COMMIT ||
                    (memory.Protect &
                     (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                    return false;
                }
                const auto* regionEnd =
                    static_cast<const std::byte*>(
                        memory.BaseAddress) +
                    memory.RegionSize;
                if (regionEnd <= cursor) {
                    return false;
                }
                cursor = (std::min)(regionEnd, end);
            }
            return true;
        }

        template <std::size_t Size>
        [[nodiscard]] bool Matches(
            const void* address,
            const std::array<std::uint8_t, Size>& bytes) noexcept
        {
            return IsReadable(address, bytes.size()) &&
                   std::memcmp(
                       address,
                       bytes.data(),
                       bytes.size()) == 0;
        }

        [[nodiscard]] bool CallTargets(
            std::uintptr_t callsite,
            std::uintptr_t target) noexcept
        {
            if (!IsReadable(
                    reinterpret_cast<const void*>(callsite),
                    5)) {
                return false;
            }
            const auto* instruction =
                reinterpret_cast<const std::uint8_t*>(
                    callsite);
            if (instruction[0] != 0xE8) {
                return false;
            }
            std::int32_t displacement = 0;
            std::memcpy(
                &displacement,
                instruction + 1,
                sizeof(displacement));
            return EngineStereoSubmissionPolicy::
                       RelativeCallTarget(
                           callsite,
                           displacement) ==
                   target;
        }

        void RestoreCall(
            std::uintptr_t callsite,
            const std::array<std::uint8_t, 5>& bytes) noexcept
        {
            if (callsite == 0) {
                return;
            }
            try {
                REL::safe_write(
                    callsite,
                    bytes.data(),
                    bytes.size());
            } catch (...) {
                logger::critical(
                    "PrismaUI could not roll back a failed stereo hook transaction");
            }
        }

        [[nodiscard]] PrismaUI::Core::SubmittedTextureLayout
            VerifiedLayout() noexcept
        {
            using namespace EngineStereoSubmissionPolicy;
            PrismaUI::Core::SubmittedTextureLayout layout;
            layout.left = {
                true,
                kLeftBounds.uMin,
                kLeftBounds.vMin,
                kLeftBounds.uMax,
                kLeftBounds.vMax
            };
            layout.right = {
                true,
                kRightBounds.uMin,
                kRightBounds.vMin,
                kRightBounds.uMax,
                kRightBounds.vMax
            };
            layout.stereoPairVerified = true;
            return layout;
        }

        [[nodiscard]] bool EnsureMinHook() noexcept
        {
            const auto result = MH_Initialize();
            if (result == MH_OK ||
                result == MH_ERROR_ALREADY_INITIALIZED) {
                return true;
            }
            logger::error(
                "PrismaUI could not initialize its resize hook ({})",
                static_cast<int>(result));
            return false;
        }
    }

    void EngineVRHooks::HookSubmitStereoTexture(
        ID3D11Texture2D* texture) noexcept
    {
        class FrameEpoch final
        {
        public:
            ~FrameEpoch() noexcept
            {
                PrismaUI::SceneDepthCapture::
                    AdvanceSubmittedFrame();
            }
        } frameEpoch;

        try {
            if (texture) {
                PrismaUI::Core::RenderSubmittedTexture(
                    texture,
                    VerifiedLayout());
            }
        } catch (...) {
            try {
                logger::error(
                    "PrismaUI contained an exception at the stereo submission boundary");
            } catch (...) {
            }
        }

        if (g_originalSubmit) {
            g_originalSubmit(texture);
        }
    }

    bool EngineVRHooks::Install() noexcept
    {
        std::lock_guard lock(g_installMutex);
        if (g_engineInstalled.load(
                std::memory_order_acquire)) {
            return true;
        }
        if (!IsExactSupportedRuntime()) {
            logger::critical(
                "PrismaUI stereo composition only supports Fallout 4 VR 1.2.72");
            return false;
        }

        using namespace EngineStereoSubmissionPolicy;
        std::uintptr_t fullCallsite = 0;
        std::uintptr_t specialCallsite = 0;
        auto fullPatched = false;
        auto specialPatched = false;
        try {
            const REL::Relocation<std::uintptr_t> fullCall{
                REL::Offset(kFullSubmitCallsiteRva)
            };
            const REL::Relocation<std::uintptr_t> specialCall{
                REL::Offset(kSpecialSubmitCallsiteRva)
            };
            const REL::Relocation<std::uintptr_t> submit{
                REL::Offset(kSubmitStereoTextureRva)
            };
            fullCallsite = fullCall.address();
            specialCallsite = specialCall.address();
            const auto submitAddress = submit.address();

            const auto guarded =
                fullCallsite >= kCallsitePrefixSize &&
                specialCallsite >= kCallsitePrefixSize &&
                Matches(
                    reinterpret_cast<const void*>(
                        fullCallsite -
                        kCallsitePrefixSize),
                    kFullSubmitBoundary) &&
                Matches(
                    reinterpret_cast<const void*>(
                        specialCallsite -
                        kCallsitePrefixSize),
                    kSpecialSubmitBoundary) &&
                Matches(
                    reinterpret_cast<const void*>(
                        submitAddress),
                    kSubmitStereoTexturePrologue) &&
                CallTargets(
                    fullCallsite,
                    submitAddress) &&
                CallTargets(
                    specialCallsite,
                    submitAddress);
            if (!guarded) {
                logger::critical(
                    "PrismaUI stereo submission guards do not match Fallout 4 VR 1.2.72");
                return false;
            }

            g_originalSubmit =
                reinterpret_cast<SubmitStereoTexture>(
                    submitAddress);
            auto& trampoline = F4SE::GetTrampoline();
            const auto fullOriginal =
                trampoline.write_call<5>(
                    fullCallsite,
                    HookSubmitStereoTexture);
            fullPatched = true;
            if (fullOriginal != submitAddress) {
                RestoreCall(
                    fullCallsite,
                    kFullSubmitOriginalCall);
                fullPatched = false;
                g_originalSubmit = nullptr;
                return false;
            }

            const auto specialOriginal =
                trampoline.write_call<5>(
                    specialCallsite,
                    HookSubmitStereoTexture);
            specialPatched = true;
            if (specialOriginal != submitAddress) {
                RestoreCall(
                    specialCallsite,
                    kSpecialSubmitOriginalCall);
                specialPatched = false;
                RestoreCall(
                    fullCallsite,
                    kFullSubmitOriginalCall);
                fullPatched = false;
                g_originalSubmit = nullptr;
                return false;
            }

            g_engineInstalled.store(
                true,
                std::memory_order_release);
            logger::info(
                "PrismaUI installed guarded FO4VR stereo composition");
            return true;
        } catch (...) {
            if (specialPatched) {
                RestoreCall(
                    specialCallsite,
                    kSpecialSubmitOriginalCall);
            }
            if (fullPatched) {
                RestoreCall(
                    fullCallsite,
                    kFullSubmitOriginalCall);
            }
            g_originalSubmit = nullptr;
            logger::critical(
                "PrismaUI stereo hook installation failed");
            return false;
        }
    }

    bool EngineVRHooks::IsInstalled() noexcept
    {
        return g_engineInstalled.load(
            std::memory_order_acquire);
    }

    HRESULT APIENTRY D3DHooks::HookResizeBuffers(
        IDXGISwapChain* swapChain,
        UINT bufferCount,
        UINT width,
        UINT height,
        DXGI_FORMAT newFormat,
        UINT flags) noexcept
    {
        try {
            PrismaUI::Core::OnResizeBuffers();
        } catch (...) {
        }
        return g_originalResize ?
            g_originalResize(
                swapChain,
                bufferCount,
                width,
                height,
                newFormat,
                flags) :
            E_FAIL;
    }

    bool D3DHooks::Install() noexcept
    {
        std::lock_guard lock(g_installMutex);
        if (g_resizeInstalled.load(
                std::memory_order_acquire)) {
            return true;
        }
        if (!IsExactSupportedRuntime()) {
            return false;
        }

        const auto rendererData =
            RE::BSGraphics::RendererData::GetSingleton();
        const auto swapChain = rendererData ?
            reinterpret_cast<IDXGISwapChain*>(
                rendererData->renderWindow[0].swapChain) :
            nullptr;
        if (!swapChain || !EnsureMinHook()) {
            return false;
        }

        auto** vtable =
            *reinterpret_cast<void***>(swapChain);
        if (!vtable || !vtable[13]) {
            return false;
        }
        g_resizeTarget = vtable[13];

        const auto create = MH_CreateHook(
            g_resizeTarget,
            reinterpret_cast<void*>(&HookResizeBuffers),
            reinterpret_cast<void**>(&g_originalResize));
        if (create != MH_OK || !g_originalResize) {
            if (create == MH_OK) {
                (void)MH_RemoveHook(g_resizeTarget);
            }
            g_resizeTarget = nullptr;
            g_originalResize = nullptr;
            logger::error(
                "PrismaUI could not create its resize hook ({})",
                static_cast<int>(create));
            return false;
        }
        const auto enabled =
            MH_EnableHook(g_resizeTarget);
        if (enabled != MH_OK) {
            (void)MH_DisableHook(g_resizeTarget);
            (void)MH_RemoveHook(g_resizeTarget);
            g_resizeTarget = nullptr;
            g_originalResize = nullptr;
            logger::error(
                "PrismaUI could not enable its resize hook ({})",
                static_cast<int>(enabled));
            return false;
        }

        g_resizeInstalled.store(
            true,
            std::memory_order_release);
        logger::info(
            "PrismaUI installed swap-chain resize invalidation");
        return true;
    }

    void D3DHooks::Uninstall() noexcept
    {
        std::lock_guard lock(g_installMutex);
        PrismaUI::SceneDepthCapture::Uninstall();
        if (g_resizeTarget &&
            g_resizeInstalled.exchange(
                false,
                std::memory_order_acq_rel)) {
            (void)MH_DisableHook(g_resizeTarget);
            (void)MH_RemoveHook(g_resizeTarget);
        }
        g_resizeTarget = nullptr;
        g_originalResize = nullptr;
    }
}
