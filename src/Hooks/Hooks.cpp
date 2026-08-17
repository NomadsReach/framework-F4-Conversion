#include "Hooks.h"

#include <atomic>
#include <mutex>

#include "PrismaUI/Core.h"

namespace Hooks {
namespace {

std::mutex g_hookMutex;
std::atomic<bool> g_installed{false};
void* g_presentTarget = nullptr;
void* g_resizeTarget = nullptr;

void RemoveCreatedHooks()
{
    if (g_presentTarget) {
        MH_DisableHook(g_presentTarget);
        MH_RemoveHook(g_presentTarget);
    }
    if (g_resizeTarget) {
        MH_DisableHook(g_resizeTarget);
        MH_RemoveHook(g_resizeTarget);
    }
    g_presentTarget = nullptr;
    g_resizeTarget = nullptr;
    D3DHooks::oPresent = nullptr;
    D3DHooks::oResizeBuffers = nullptr;
}

}

HRESULT APIENTRY D3DHooks::HookPresent(REX::W32::IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    thread_local bool insideHook = false;
    if (!oPresent) return E_FAIL;
    if (insideHook) return oPresent(swapChain, syncInterval, flags);

    struct Guard {
        bool& value;
        explicit Guard(bool& flag) : value(flag) { value = true; }
        ~Guard() { value = false; }
    } guard{insideHook};

    try {
        PrismaUI::Core::D3DPresent(0);
    } catch (const std::exception& e) {
        logger::error("Present hook frame failed: {}", e.what());
    } catch (...) {
        logger::error("Present hook frame failed");
    }

    return oPresent(swapChain, syncInterval, flags);
}

HRESULT APIENTRY D3DHooks::HookResizeBuffers(REX::W32::IDXGISwapChain* swapChain, UINT bufferCount, UINT width,
                                              UINT height, REX::W32::DXGI_FORMAT format, UINT swapChainFlags)
{
    if (!oResizeBuffers) return E_FAIL;

    try {
        PrismaUI::Core::OnResizeBuffers();
    } catch (const std::exception& e) {
        logger::error("ResizeBuffers cleanup failed: {}", e.what());
    } catch (...) {
        logger::error("ResizeBuffers cleanup failed");
    }

    return oResizeBuffers(swapChain, bufferCount, width, height, format, swapChainFlags);
}

void D3DHooks::Install()
{
    std::lock_guard lock(g_hookMutex);
    if (g_installed.load(std::memory_order_acquire)) return;

    auto* rendererData = RE::BSGraphics::GetRendererData();
    if (!rendererData || !rendererData->renderWindow[0].swapChain) {
        logger::critical("D3D hook install failed: swap chain unavailable");
        return;
    }

    auto* swapChain = rendererData->renderWindow[0].swapChain;
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!vtable || !vtable[8] || !vtable[13]) {
        logger::critical("D3D hook install failed: invalid swap-chain vtable");
        return;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        logger::critical("D3D hook install failed: MH_Initialize={}", static_cast<int>(initStatus));
        return;
    }

    g_presentTarget = vtable[8];
    g_resizeTarget = vtable[13];

    if (MH_CreateHook(g_presentTarget, &HookPresent, reinterpret_cast<void**>(&oPresent)) != MH_OK) {
        logger::critical("D3D hook install failed: Present create");
        RemoveCreatedHooks();
        return;
    }

    if (MH_CreateHook(g_resizeTarget, &HookResizeBuffers, reinterpret_cast<void**>(&oResizeBuffers)) != MH_OK) {
        logger::critical("D3D hook install failed: ResizeBuffers create");
        RemoveCreatedHooks();
        return;
    }

    if (MH_EnableHook(g_presentTarget) != MH_OK) {
        logger::critical("D3D hook install failed: Present enable");
        RemoveCreatedHooks();
        return;
    }

    if (MH_EnableHook(g_resizeTarget) != MH_OK) {
        logger::critical("D3D hook install failed: ResizeBuffers enable");
        RemoveCreatedHooks();
        return;
    }

    g_installed.store(true, std::memory_order_release);
    logger::info("D3D hooks installed");
}

void D3DHooks::Uninstall()
{
    std::lock_guard lock(g_hookMutex);
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
    RemoveCreatedHooks();
    MH_Uninitialize();
    logger::info("D3D hooks uninstalled");
}

}
