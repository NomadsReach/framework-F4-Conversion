#include "Inspector.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>

#include "Core.h"
#include "InputHandler.h"
#include "Utils/DllLoader.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace PrismaUI::Inspector {
using namespace Core;

namespace {

constexpr size_t kBytesPerPixel = 4;
std::once_flag g_assetCheck;
std::atomic<bool> g_assetsAvailable{false};

std::shared_ptr<PrismaView> GetView(Core::PrismaViewId viewId)
{
    std::shared_lock lock(viewsMutex);
    auto it = views.find(viewId);
    if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) return nullptr;
    return it->second;
}

template <class F>
void RunOnUltralight(F&& fn)
{
    if (ultralightThread.IsWorkerThread()) {
        std::forward<F>(fn)();
        return;
    }

    try {
        ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::HIGH, std::forward<F>(fn)).get();
    } catch (const std::exception& e) {
        logger::error("Inspector dispatch failed: {}", e.what());
    } catch (...) {
        logger::error("Inspector dispatch failed");
    }
}

}

void EnsureInspectorAssetsAvailability()
{
    const auto path = Utils::GetBasePath() / "inspector" / "Main.html";
    std::call_once(g_assetCheck, [path] {
        try {
            g_assetsAvailable.store(std::filesystem::exists(path), std::memory_order_release);
        } catch (...) {
            g_assetsAvailable.store(false, std::memory_order_release);
        }
    });
}

bool AreInspectorAssetsAvailable()
{
    EnsureInspectorAssetsAvailability();
    return g_assetsAvailable.load(std::memory_order_acquire);
}

void ReleaseInspectorTexture(PrismaView* viewData)
{
    if (!viewData) return;

    if (viewData->inspectorTextureView) {
        viewData->inspectorTextureView->Release();
        viewData->inspectorTextureView = nullptr;
    }
    if (viewData->inspectorTexture) {
        viewData->inspectorTexture->Release();
        viewData->inspectorTexture = nullptr;
    }
    viewData->inspectorTextureWidth = 0;
    viewData->inspectorTextureHeight = 0;
}

void ClearInspectorBuffers(PrismaView* viewData)
{
    if (!viewData) return;

    {
        std::lock_guard lock(viewData->inspectorBufferMutex);
        viewData->inspectorPixelBuffer.clear();
        viewData->inspectorPixelBuffer.shrink_to_fit();
        viewData->inspectorBufferWidth = 0;
        viewData->inspectorBufferHeight = 0;
        viewData->inspectorBufferStride = 0;
    }
    viewData->inspectorFrameReady.store(false, std::memory_order_release);
    viewData->inspectorPointerHover.store(false, std::memory_order_release);
}

void DestroyInspectorResources(PrismaView* viewData)
{
    if (!viewData) return;
    viewData->inspectorVisible.store(false, std::memory_order_release);
    ClearInspectorBuffers(viewData);
    viewData->pendingResourceRelease.store(true, std::memory_order_release);
}

void CreateInspectorView(const PrismaViewId& viewId)
{
    if (!AreInspectorAssetsAvailable()) {
        logger::warn("View [{}]: Ultralight inspector assets are unavailable", viewId);
        return;
    }

    auto viewData = GetView(viewId);
    if (!viewData) return;

    RunOnUltralight([viewData] {
        if (viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView ||
            viewData->inspectorView) {
            return;
        }
        viewData->ultralightView->CreateLocalInspectorView();
    });
}

void SetInspectorVisibility(const PrismaViewId& viewId, bool visible)
{
    auto viewData = GetView(viewId);
    if (!viewData) return;

    RunOnUltralight([viewData, viewId, visible] {
        if (viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) return;

        if (visible && !viewData->inspectorView) {
            viewData->ultralightView->CreateLocalInspectorView();
        }
        if (visible && !viewData->inspectorView) return;

        viewData->inspectorVisible.store(visible, std::memory_order_release);
        viewData->inspectorPointerHover.store(false, std::memory_order_release);

        if (visible) {
            viewData->inspectorView->Focus();
            if (viewData->ultralightView->HasFocus()) viewData->ultralightView->Unfocus();
            return;
        }

        if (viewData->inspectorView && viewData->inspectorView->HasFocus()) viewData->inspectorView->Unfocus();
        if (InputHandler::IsInputCaptureActiveForView(viewId) && !viewData->ultralightView->HasFocus()) {
            viewData->ultralightView->Focus();
        }
    });
}

bool IsInspectorVisible(const PrismaViewId& viewId)
{
    auto viewData = GetView(viewId);
    return viewData && viewData->inspectorVisible.load(std::memory_order_acquire);
}

void SetInspectorBounds(const PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width, uint32_t height)
{
    auto viewData = GetView(viewId);
    if (!viewData) return;

    width = std::max<uint32_t>(width, 32u);
    height = std::max<uint32_t>(height, 32u);

    const uint32_t screenWidth = screenSize.width.load(std::memory_order_acquire);
    const uint32_t screenHeight = screenSize.height.load(std::memory_order_acquire);
    const float maxX = std::max(0.0f, static_cast<float>(screenWidth ? screenWidth : width) - width);
    const float maxY = std::max(0.0f, static_cast<float>(screenHeight ? screenHeight : height) - height);
    const float x = std::clamp(topLeftX, 0.0f, maxX);
    const float y = std::clamp(topLeftY, 0.0f, maxY);

    viewData->inspectorPosX.store(x, std::memory_order_release);
    viewData->inspectorPosY.store(y, std::memory_order_release);
    viewData->inspectorDisplayWidth.store(width, std::memory_order_release);
    viewData->inspectorDisplayHeight.store(height, std::memory_order_release);
    viewData->inspectorPointerHover.store(false, std::memory_order_release);

    RunOnUltralight([viewData, width, height] {
        if (!viewData->isDestroying.load(std::memory_order_acquire) && viewData->inspectorView) {
            viewData->inspectorView->Resize(width, height);
        }
    });
}

void RenderInspectorView(std::shared_ptr<PrismaView> viewData)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) ||
        !viewData->inspectorVisible.load(std::memory_order_acquire) || viewData->isHidden.load(std::memory_order_acquire) ||
        !viewData->inspectorView) {
        return;
    }
    CopyInspectorBitmapToBuffer(std::move(viewData));
}

void CopyInspectorBitmapToBuffer(std::shared_ptr<PrismaView> viewData)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->inspectorView) return;

    Surface* surface = viewData->inspectorView->surface();
    if (!surface) return;

    auto* bitmapSurface = static_cast<BitmapSurface*>(surface);
    RefPtr<Bitmap> bitmap = bitmapSurface->bitmap();
    if (!bitmap || bitmap->IsEmpty()) return;

    const void* pixels = bitmap->LockPixels();
    if (!pixels) return;

    const uint32_t width = bitmap->width();
    const uint32_t height = bitmap->height();
    const uint32_t stride = bitmap->row_bytes();
    if (width == 0 || height == 0 || stride == 0 ||
        static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / static_cast<size_t>(stride)) {
        bitmap->UnlockPixels();
        return;
    }

    const size_t dataSize = static_cast<size_t>(stride) * static_cast<size_t>(height);
    try {
        std::lock_guard lock(viewData->inspectorBufferMutex);
        viewData->inspectorPixelBuffer.resize(dataSize);
        std::memcpy(viewData->inspectorPixelBuffer.data(), pixels, dataSize);
        viewData->inspectorBufferWidth = width;
        viewData->inspectorBufferHeight = height;
        viewData->inspectorBufferStride = stride;
        viewData->inspectorFrameReady.store(true, std::memory_order_release);
    } catch (const std::exception& e) {
        logger::error("View [{}]: inspector buffer update failed: {}", viewData->id, e.what());
        ClearInspectorBuffers(viewData.get());
    }

    bitmap->UnlockPixels();
}

void CopyInspectorPixelsToTexture(PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !pixels || !d3dContext || !d3dDevice ||
        width == 0 || height == 0) {
        return;
    }

    const size_t rowBytes = static_cast<size_t>(width) * kBytesPerPixel;
    if (rowBytes > std::numeric_limits<uint32_t>::max() || stride < rowBytes) return;

    if (!viewData->inspectorTexture || viewData->inspectorTextureWidth != width ||
        viewData->inspectorTextureHeight != height) {
        ReleaseInspectorTexture(viewData);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &viewData->inspectorTexture);
        if (FAILED(hr)) return;

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        hr = d3dDevice->CreateShaderResourceView(viewData->inspectorTexture, &srv, &viewData->inspectorTextureView);
        if (FAILED(hr)) {
            ReleaseInspectorTexture(viewData);
            return;
        }

        viewData->inspectorTextureWidth = width;
        viewData->inspectorTextureHeight = height;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = d3dContext->Map(viewData->inspectorTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || mapped.RowPitch < rowBytes) {
        if (SUCCEEDED(hr)) d3dContext->Unmap(viewData->inspectorTexture, 0);
        return;
    }

    const auto* source = static_cast<const std::byte*>(pixels);
    auto* destination = static_cast<std::byte*>(mapped.pData);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * mapped.RowPitch,
                    source + static_cast<size_t>(row) * stride, rowBytes);
    }
    d3dContext->Unmap(viewData->inspectorTexture, 0);
}

}
