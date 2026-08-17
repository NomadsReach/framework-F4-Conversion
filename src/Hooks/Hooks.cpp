#include "Hooks.h"

#include "PrismaUI/Core.h"

namespace Hooks {
    HRESULT APIENTRY D3DHooks::HookPresent(REX::W32::IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
        PrismaUI::Core::D3DPresent(0);
        return oPresent(swapChain, syncInterval, flags);
    }

    HRESULT APIENTRY D3DHooks::HookResizeBuffers(REX::W32::IDXGISwapChain* swapChain, UINT bufferCount, UINT width,
                                                 UINT height, REX::W32::DXGI_FORMAT newFormat,
                                                 UINT swapChainFlags) {
        PrismaUI::Core::OnResizeBuffers();
        return oResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
    }

    void D3DHooks::Install() {
        auto* rendererData = RE::BSGraphics::GetRendererData();
        if (!rendererData || !rendererData->renderWindow[0].swapChain) {
            logger::critical("D3DHooks::Install: Failed to get IDXGISwapChain from BSGraphics!");
            return;
        }

        REX::W32::IDXGISwapChain* swapChain = rendererData->renderWindow[0].swapChain;
        void** vtable = *reinterpret_cast<void***>(swapChain);

        auto mhStatus = MH_Initialize();
        if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
            logger::critical("D3DHooks::Install: MH_Initialize failed!");
            return;
        }

        if (MH_CreateHook(vtable[8], &HookPresent, reinterpret_cast<LPVOID*>(&oPresent)) != MH_OK) {
            logger::critical("D3DHooks::Install: Failed to hook IDXGISwapChain::Present!");
            return;
        }

        if (MH_CreateHook(vtable[13], &HookResizeBuffers, reinterpret_cast<LPVOID*>(&oResizeBuffers)) != MH_OK) {
            logger::critical("D3DHooks::Install: Failed to hook IDXGISwapChain::ResizeBuffers!");
            return;
        }

        if (MH_EnableHook(vtable[8]) != MH_OK) {
            logger::critical("D3DHooks::Install: MH_EnableHook(Present) failed!");
            return;
        }

        if (MH_EnableHook(vtable[13]) != MH_OK) {
            logger::critical("D3DHooks::Install: MH_EnableHook(ResizeBuffers) failed!");
            return;
        }

        logger::info("D3D hooks installed: Present(vtable[8]) + ResizeBuffers(vtable[13])");
    }

    void D3DHooks::Uninstall() {
        if (MH_DisableHook(MH_ALL_HOOKS) != MH_OK) {
            logger::warn("D3DHooks::Uninstall: MH_DisableHook failed.");
        }
        if (MH_Uninitialize() != MH_OK) {
            logger::warn("D3DHooks::Uninstall: MH_Uninitialize failed.");
        }
        logger::info("D3D hooks uninstalled.");
    }
}
