#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace Hooks
{
    class EngineVRHooks final
    {
    public:
        [[nodiscard]] static bool Install() noexcept;
        [[nodiscard]] static bool IsInstalled() noexcept;

    private:
        static void HookSubmitStereoTexture(
            ID3D11Texture2D* texture) noexcept;
    };

    class D3DHooks final
    {
    public:
        [[nodiscard]] static bool Install() noexcept;
        static void Uninstall() noexcept;

    private:
        static HRESULT APIENTRY HookResizeBuffers(
            IDXGISwapChain* swapChain,
            UINT bufferCount,
            UINT width,
            UINT height,
            DXGI_FORMAT newFormat,
            UINT flags) noexcept;
    };
}
