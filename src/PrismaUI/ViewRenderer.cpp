#include "ViewRenderer.h"

#include <DirectXTK/SimpleMath.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "Core.h"
#include "InputHandler.h"
#include "Inspector.h"

namespace PrismaUI::ViewRenderer {
using namespace Core;

namespace {

class D3DStateGuard {
public:
    explicit D3DStateGuard(ID3D11DeviceContext* context) : context_(context)
    {
        if (!context_) return;
        context_->OMGetBlendState(blend_.GetAddressOf(), blendFactor_, &sampleMask_);
        context_->OMGetDepthStencilState(depth_.GetAddressOf(), &stencilRef_);
        context_->RSGetState(rasterizer_.GetAddressOf());
    }

    ~D3DStateGuard()
    {
        if (!context_) return;
        context_->OMSetBlendState(blend_.Get(), blendFactor_, sampleMask_);
        context_->OMSetDepthStencilState(depth_.Get(), stencilRef_);
        context_->RSSetState(rasterizer_.Get());
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    FLOAT blendFactor_[4]{};
    UINT sampleMask_ = 0;
    UINT stencilRef_ = 0;
};

struct DrawEntry {
    int order = 0;
    std::shared_ptr<PrismaView> view;
};

}

void UpdateLogic()
{
    if (renderer) renderer->Update();
}

void RenderViews()
{
    if (!renderer) return;

    std::vector<std::shared_ptr<PrismaView>> snapshot;
    {
        std::shared_lock lock(viewsMutex);
        snapshot.reserve(views.size());
        for (const auto& [id, view] : views) {
            if (view && !view->isHidden.load(std::memory_order_acquire) &&
                !view->isDestroying.load(std::memory_order_acquire)) {
                snapshot.push_back(view);
            }
        }
    }

    for (const auto& view : snapshot) RenderSingleView(view);
}

void RenderSingleView(std::shared_ptr<PrismaView> viewData)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) return;

    Surface* surfaceBase = viewData->ultralightView->surface();
    if (!surfaceBase) return;
    auto* surface = static_cast<BitmapSurface*>(surfaceBase);

    if (viewData->isLoadingFinished.load(std::memory_order_acquire) && !surface->dirty_bounds().IsEmpty()) {
        CopyBitmapToBuffer(viewData);
        surface->ClearDirtyBounds();
    }

    Inspector::RenderInspectorView(std::move(viewData));
}

void CopyBitmapToBuffer(std::shared_ptr<PrismaView> viewData)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) return;

    auto* surface = static_cast<BitmapSurface*>(viewData->ultralightView->surface());
    if (!surface) return;
    RefPtr<Bitmap> bitmap = surface->bitmap();
    if (!bitmap) return;

    void* pixels = bitmap->LockPixels();
    if (!pixels) return;

    const uint32_t width = bitmap->width();
    const uint32_t height = bitmap->height();
    const uint32_t stride = bitmap->row_bytes();
    if (width == 0 || height == 0 || stride == 0 ||
        static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / static_cast<size_t>(stride)) {
        bitmap->UnlockPixels();
        return;
    }

    const size_t size = static_cast<size_t>(height) * static_cast<size_t>(stride);
    bool copied = false;
    try {
        std::lock_guard lock(viewData->bufferMutex);
        viewData->pixelBuffer.resize(size);
        std::memcpy(viewData->pixelBuffer.data(), pixels, size);
        viewData->bufferWidth = width;
        viewData->bufferHeight = height;
        viewData->bufferStride = stride;
        copied = true;
    } catch (const std::exception& e) {
        logger::error("View [{}] bitmap copy failed: {}", viewData->id, e.what());
        std::lock_guard lock(viewData->bufferMutex);
        viewData->pixelBuffer.clear();
        viewData->bufferWidth = 0;
        viewData->bufferHeight = 0;
        viewData->bufferStride = 0;
    }

    bitmap->UnlockPixels();
    viewData->newFrameReady.store(copied, std::memory_order_release);
}

void ReleaseViewTexture(PrismaView* viewData)
{
    if (!viewData) return;
    if (viewData->textureView) {
        viewData->textureView->Release();
        viewData->textureView = nullptr;
    }
    if (viewData->texture) {
        viewData->texture->Release();
        viewData->texture = nullptr;
    }
    viewData->textureWidth = 0;
    viewData->textureHeight = 0;
}

void UpdateSingleTextureFromBuffer(std::shared_ptr<PrismaView> viewData)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) return;

    if (viewData->pendingResourceRelease.exchange(false, std::memory_order_acq_rel)) {
        ReleaseViewTexture(viewData.get());
        Inspector::ReleaseInspectorTexture(viewData.get());
        return;
    }

    if (viewData->newFrameReady.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard lock(viewData->bufferMutex);
        if (!viewData->pixelBuffer.empty() && viewData->bufferWidth > 0 && viewData->bufferHeight > 0) {
            CopyPixelsToTexture(viewData.get(), viewData->pixelBuffer.data(), viewData->bufferWidth,
                                viewData->bufferHeight, viewData->bufferStride);
        }
    }

    if (viewData->inspectorVisible.load(std::memory_order_acquire) &&
        viewData->inspectorFrameReady.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard lock(viewData->inspectorBufferMutex);
        if (!viewData->inspectorPixelBuffer.empty() && viewData->inspectorBufferWidth > 0 &&
            viewData->inspectorBufferHeight > 0) {
            Inspector::CopyInspectorPixelsToTexture(viewData.get(), viewData->inspectorPixelBuffer.data(),
                                                    viewData->inspectorBufferWidth, viewData->inspectorBufferHeight,
                                                    viewData->inspectorBufferStride);
        }
    }
}

void CopyPixelsToTexture(PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !d3dDevice || !d3dContext ||
        !pixels || width == 0 || height == 0) {
        return;
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    if (rowBytes > std::numeric_limits<uint32_t>::max() || stride < rowBytes) return;

    if (!viewData->texture || viewData->textureWidth != width || viewData->textureHeight != height) {
        ReleaseViewTexture(viewData);

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

        HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &viewData->texture);
        if (FAILED(hr)) return;

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        hr = d3dDevice->CreateShaderResourceView(viewData->texture, &srv, &viewData->textureView);
        if (FAILED(hr)) {
            ReleaseViewTexture(viewData);
            return;
        }

        viewData->textureWidth = width;
        viewData->textureHeight = height;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = d3dContext->Map(viewData->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || mapped.RowPitch < rowBytes) {
        if (SUCCEEDED(hr)) d3dContext->Unmap(viewData->texture, 0);
        return;
    }

    const auto* source = static_cast<const std::byte*>(pixels);
    auto* destination = static_cast<std::byte*>(mapped.pData);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * mapped.RowPitch,
                    source + static_cast<size_t>(row) * stride, rowBytes);
    }
    d3dContext->Unmap(viewData->texture, 0);
}

void DrawCursor()
{
    if (!spriteBatch || !commonStates || !cursorTexture || !InputHandler::IsAnyInputCaptureActive()) return;

    D3DStateGuard state(d3dContext);
    try {
        spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());
        const DirectX::SimpleMath::Vector2 position(static_cast<float>(InputHandler::GetLastCursorX()),
                                                     static_cast<float>(InputHandler::GetLastCursorY()));
        spriteBatch->Draw(cursorTexture.Get(), position);
        spriteBatch->End();
    } catch (const std::exception& e) {
        logger::error("Cursor draw failed: {}", e.what());
    } catch (...) {
        logger::error("Cursor draw failed");
    }
}

void DrawViews()
{
    if (!spriteBatch || !commonStates) return;

    std::vector<DrawEntry> entries;
    {
        std::shared_lock lock(viewsMutex);
        entries.reserve(views.size());
        for (const auto& [id, view] : views) {
            if (!view || view->isDestroying.load(std::memory_order_acquire) ||
                view->isHidden.load(std::memory_order_acquire) || view->pendingResourceRelease.load(std::memory_order_acquire) ||
                !view->textureView) {
                continue;
            }
            entries.push_back({view->order, view});
        }
    }

    if (entries.empty()) return;
    std::sort(entries.begin(), entries.end(), [](const DrawEntry& a, const DrawEntry& b) { return a.order < b.order; });

    D3DStateGuard state(d3dContext);
    try {
        spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());
        for (const auto& entry : entries) DrawSingleTexture(entry.view);
        spriteBatch->End();
    } catch (const std::exception& e) {
        logger::error("View draw failed: {}", e.what());
    } catch (...) {
        logger::error("View draw failed");
    }
}

void DrawSingleTexture(std::shared_ptr<PrismaView> viewData)
{
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->textureView ||
        viewData->textureWidth == 0 || viewData->textureHeight == 0) {
        return;
    }

    const RECT sourceRect{0, 0, static_cast<LONG>(viewData->textureWidth), static_cast<LONG>(viewData->textureHeight)};
    spriteBatch->Draw(viewData->textureView, DirectX::SimpleMath::Vector2::Zero, &sourceRect, DirectX::Colors::White);

    if (!viewData->inspectorVisible.load(std::memory_order_acquire) || !viewData->inspectorTextureView ||
        viewData->inspectorTextureWidth == 0 || viewData->inspectorTextureHeight == 0) {
        return;
    }

    const DirectX::SimpleMath::Vector2 inspectorPosition(
        viewData->inspectorPosX.load(std::memory_order_acquire),
        viewData->inspectorPosY.load(std::memory_order_acquire));
    const RECT inspectorRect{0, 0, static_cast<LONG>(viewData->inspectorTextureWidth),
                             static_cast<LONG>(viewData->inspectorTextureHeight)};
    spriteBatch->Draw(viewData->inspectorTextureView, inspectorPosition, &inspectorRect, DirectX::Colors::White);
}

}
