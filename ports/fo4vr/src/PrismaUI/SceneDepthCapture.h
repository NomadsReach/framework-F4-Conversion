#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace PrismaUI::SceneDepthCapture
{
    struct FrameDepth
    {
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> view;
        D3D11_COMPARISON_FUNC comparison =
            D3D11_COMPARISON_LESS_EQUAL;
        std::uint64_t frameEpoch = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return view &&
                   comparison >= D3D11_COMPARISON_NEVER &&
                   comparison <= D3D11_COMPARISON_ALWAYS;
        }
    };

    [[nodiscard]] bool Install() noexcept;
    [[nodiscard]] bool IsInstalled() noexcept;
    void Uninstall() noexcept;
    void SetCaptureRequested(bool requested) noexcept;
    void SetSubmittedTarget(ID3D11Texture2D* texture) noexcept;
    [[nodiscard]] FrameDepth AcquireForSubmittedTarget(
        ID3D11Texture2D* colorTexture,
        const D3D11_TEXTURE2D_DESC& colorDescription) noexcept;
    void AdvanceSubmittedFrame() noexcept;
    void Reset() noexcept;
}
