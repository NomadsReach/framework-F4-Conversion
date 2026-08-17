#include "ViewRenderer.h"

#include <DirectXTK/SimpleMath.h>
#include <algorithm>
#include <cstring>

#include "Core.h"
#include "InputHandler.h"
#include "Inspector.h"

namespace PrismaUI::ViewRenderer {
    using namespace Core;

    void UpdateLogic() {
        if (renderer) {
            renderer->Update();
        }
    }

    void RenderViews() {
        if (!renderer) {
            return;
        }

        std::vector<std::shared_ptr<PrismaView>> viewsToRender;
        {
            std::shared_lock lock(viewsMutex);
            viewsToRender.reserve(views.size());
            for (const auto& [viewId, viewData] : views) {
                if (!viewData) {
                    logger::warn("RenderViews: Found null shared_ptr in views map for ID [{}]", viewId);
                    continue;
                }
                if (!viewData->isDestroying.load(std::memory_order_acquire) && !viewData->isHidden.load()) {
                    viewsToRender.push_back(viewData);
                }
            }
        }

        for (const auto& viewData : viewsToRender) {
            RenderSingleView(viewData);
        }
    }

    void RenderSingleView(std::shared_ptr<PrismaView> viewData) {
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) {
            return;
        }

        Surface* surfaceBase = viewData->ultralightView->surface();
        if (!surfaceBase) {
            return;
        }

        auto* surface = static_cast<BitmapSurface*>(surfaceBase);
        if (viewData->isLoadingFinished.load() && !surface->dirty_bounds().IsEmpty()) {
            CopyBitmapToBuffer(viewData);
            surface->ClearDirtyBounds();
        }

        Inspector::RenderInspectorView(viewData);
    }

    void CopyBitmapToBuffer(std::shared_ptr<PrismaView> viewData) {
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) {
            return;
        }

        Surface* surfaceBase = viewData->ultralightView->surface();
        if (!surfaceBase) {
            return;
        }

        auto* surface = static_cast<BitmapSurface*>(surfaceBase);
        RefPtr<Bitmap> bitmap = surface->bitmap();
        if (!bitmap) {
            return;
        }

        void* pixels = bitmap->LockPixels();
        if (!pixels) {
            logger::error("View [{}]: Failed to lock bitmap pixels.", viewData->id);
            return;
        }

        const uint32_t width = bitmap->width();
        const uint32_t height = bitmap->height();
        const uint32_t stride = bitmap->row_bytes();
        const size_t requiredSize = static_cast<size_t>(height) * stride;
        if (width == 0 || height == 0 || requiredSize == 0) {
            bitmap->UnlockPixels();
            return;
        }

        bool success = false;
        {
            std::lock_guard lock(viewData->bufferMutex);
            if (!viewData->isDestroying.load(std::memory_order_acquire)) {
                try {
                    if (viewData->pixelBuffer.size() != requiredSize) {
                        viewData->pixelBuffer.resize(requiredSize);
                    }
                    std::memcpy(viewData->pixelBuffer.data(), pixels, requiredSize);
                    viewData->bufferWidth = width;
                    viewData->bufferHeight = height;
                    viewData->bufferStride = stride;
                    success = true;
                } catch (const std::exception& e) {
                    logger::error("View [{}]: Exception during pixel buffer copy/resize: {}", viewData->id,
                                  e.what());
                    viewData->pixelBuffer.clear();
                    viewData->pixelBuffer.shrink_to_fit();
                    viewData->bufferWidth = 0;
                    viewData->bufferHeight = 0;
                    viewData->bufferStride = 0;
                }
            }
        }

        bitmap->UnlockPixels();
        viewData->newFrameReady.store(success && !viewData->isDestroying.load(std::memory_order_acquire));
    }

    void ReleaseViewTexture(PrismaView* viewData) {
        if (!viewData) {
            return;
        }

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

    void UpdateSingleTextureFromBuffer(std::shared_ptr<PrismaView> viewData) {
        if (!viewData) {
            return;
        }

        if (viewData->pendingResourceRelease.exchange(false)) {
            ReleaseViewTexture(viewData.get());
            Inspector::ReleaseInspectorTexture(viewData.get());
        }

        if (viewData->isDestroying.load(std::memory_order_acquire)) {
            return;
        }

        if (viewData->newFrameReady.exchange(false)) {
            std::lock_guard lock(viewData->bufferMutex);
            if (!viewData->isDestroying.load(std::memory_order_acquire) && !viewData->pixelBuffer.empty() &&
                viewData->bufferWidth > 0 && viewData->bufferHeight > 0) {
                CopyPixelsToTexture(viewData.get(), viewData->pixelBuffer.data(), viewData->bufferWidth,
                                    viewData->bufferHeight, viewData->bufferStride);
            }
        }

        if (viewData->inspectorVisible.load() && viewData->inspectorFrameReady.exchange(false)) {
            std::lock_guard inspectorLock(viewData->inspectorBufferMutex);
            if (!viewData->isDestroying.load(std::memory_order_acquire) &&
                !viewData->inspectorPixelBuffer.empty() && viewData->inspectorBufferWidth > 0 &&
                viewData->inspectorBufferHeight > 0) {
                Inspector::CopyInspectorPixelsToTexture(viewData.get(), viewData->inspectorPixelBuffer.data(),
                                                        viewData->inspectorBufferWidth,
                                                        viewData->inspectorBufferHeight,
                                                        viewData->inspectorBufferStride);
            }
        }
    }

    void CopyPixelsToTexture(PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride) {
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !d3dDevice || !d3dContext ||
            !pixels || width == 0 || height == 0) {
            return;
        }

        if (!viewData->texture || viewData->textureWidth != width || viewData->textureHeight != height) {
            logger::debug("View [{}]: Creating/Recreating texture ({}x{})", viewData->id, width, height);
            ReleaseViewTexture(viewData);

            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_DYNAMIC;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            HRESULT result = d3dDevice->CreateTexture2D(&textureDesc, nullptr, &viewData->texture);
            if (FAILED(result)) {
                logger::critical("View [{}]: Failed to create texture! HR={:#X}", viewData->id, result);
                ReleaseViewTexture(viewData);
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
            viewDesc.Format = textureDesc.Format;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;

            result = d3dDevice->CreateShaderResourceView(viewData->texture, &viewDesc, &viewData->textureView);
            if (FAILED(result)) {
                logger::critical("View [{}]: Failed to create SRV! HR={:#X}", viewData->id, result);
                ReleaseViewTexture(viewData);
                return;
            }

            viewData->textureWidth = width;
            viewData->textureHeight = height;
            logger::debug("View [{}]: Texture/SRV created/resized.", viewData->id);
        }

        if (viewData->isDestroying.load(std::memory_order_acquire)) {
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mappedResource = {};
        HRESULT result = d3dContext->Map(viewData->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(result)) {
            logger::error("View [{}]: Failed to map texture! HR={:#X}", viewData->id, result);
            return;
        }

        auto* source = static_cast<std::byte*>(pixels);
        auto* destination = static_cast<std::byte*>(mappedResource.pData);
        const uint32_t destinationPitch = mappedResource.RowPitch;

        if (destinationPitch == stride) {
            std::memcpy(destination, source, static_cast<size_t>(height) * stride);
        } else {
            for (uint32_t row = 0; row < height; ++row) {
                std::memcpy(destination + row * destinationPitch, source + row * stride, stride);
            }
        }

        d3dContext->Unmap(viewData->texture, 0);
    }

    void DrawCursor() {
        if (!spriteBatch || !commonStates || !cursorTexture || !d3dContext) {
            return;
        }
        if (!InputHandler::IsAnyInputCaptureActive()) {
            return;
        }

        // Fallout 4 restores cursor visibility while unpaused, so suppress the engine cursor every frame.
        auto ui = RE::UI::GetSingleton();
        if (ui) {
            auto cursorMenu = ui->GetMenu("CursorMenu");
            if (cursorMenu && cursorMenu->uiMovie) {
                cursorMenu->uiMovie->SetVisible(false);
            }
        }

        ID3D11BlendState* backupBlendState = nullptr;
        FLOAT backupBlendFactor[4] = {};
        UINT backupSampleMask = 0;
        ID3D11DepthStencilState* backupDepthStencilState = nullptr;
        UINT backupStencilRef = 0;
        ID3D11RasterizerState* backupRasterizerState = nullptr;

        d3dContext->OMGetBlendState(&backupBlendState, backupBlendFactor, &backupSampleMask);
        d3dContext->OMGetDepthStencilState(&backupDepthStencilState, &backupStencilRef);
        d3dContext->RSGetState(&backupRasterizerState);

        spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());
        DirectX::SimpleMath::Vector2 position(static_cast<float>(InputHandler::GetLastCursorX()),
                                              static_cast<float>(InputHandler::GetLastCursorY()));
        spriteBatch->Draw(cursorTexture.Get(), position);
        spriteBatch->End();

        d3dContext->OMSetBlendState(backupBlendState, backupBlendFactor, backupSampleMask);
        d3dContext->OMSetDepthStencilState(backupDepthStencilState, backupStencilRef);
        d3dContext->RSSetState(backupRasterizerState);

        if (backupBlendState) {
            backupBlendState->Release();
        }
        if (backupDepthStencilState) {
            backupDepthStencilState->Release();
        }
        if (backupRasterizerState) {
            backupRasterizerState->Release();
        }
    }

    void DrawViews() {
        if (!spriteBatch || !commonStates || !d3dContext) {
            return;
        }

        std::vector<std::shared_ptr<PrismaView>> viewsToDraw;
        {
            std::shared_lock lock(viewsMutex);
            viewsToDraw.reserve(views.size());
            for (const auto& pair : views) {
                const auto& viewData = pair.second;
                if (viewData && !viewData->isDestroying.load(std::memory_order_acquire) &&
                    !viewData->isHidden.load() && !viewData->pendingResourceRelease.load() && viewData->textureView) {
                    viewsToDraw.push_back(viewData);
                }
            }
        }

        if (viewsToDraw.empty()) {
            return;
        }

        std::sort(viewsToDraw.begin(), viewsToDraw.end(),
                  [](const std::shared_ptr<PrismaView>& left, const std::shared_ptr<PrismaView>& right) {
                      return left->order < right->order;
                  });

        try {
            ID3D11BlendState* backupBlendState = nullptr;
            FLOAT backupBlendFactor[4] = {};
            UINT backupSampleMask = 0;
            ID3D11DepthStencilState* backupDepthStencilState = nullptr;
            UINT backupStencilRef = 0;
            ID3D11RasterizerState* backupRasterizerState = nullptr;

            d3dContext->OMGetBlendState(&backupBlendState, backupBlendFactor, &backupSampleMask);
            d3dContext->OMGetDepthStencilState(&backupDepthStencilState, &backupStencilRef);
            d3dContext->RSGetState(&backupRasterizerState);

            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());
            for (const auto& viewData : viewsToDraw) {
                DrawSingleTexture(viewData);
            }
            spriteBatch->End();

            d3dContext->OMSetBlendState(backupBlendState, backupBlendFactor, backupSampleMask);
            d3dContext->OMSetDepthStencilState(backupDepthStencilState, backupStencilRef);
            d3dContext->RSSetState(backupRasterizerState);

            if (backupBlendState) {
                backupBlendState->Release();
            }
            if (backupDepthStencilState) {
                backupDepthStencilState->Release();
            }
            if (backupRasterizerState) {
                backupRasterizerState->Release();
            }
        } catch (const std::exception& e) {
            logger::error("Error during SpriteBatch drawing loop: {}", e.what());
        } catch (...) {
            logger::error("Unknown error during SpriteBatch drawing loop.");
        }
    }

    void DrawSingleTexture(std::shared_ptr<PrismaView> viewData) {
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->textureView ||
            viewData->textureWidth == 0 || viewData->textureHeight == 0) {
            return;
        }

        DirectX::SimpleMath::Vector2 position(0.0f, 0.0f);
        RECT sourceRect = {0, 0, static_cast<long>(viewData->textureWidth), static_cast<long>(viewData->textureHeight)};
        spriteBatch->Draw(viewData->textureView, position, &sourceRect, DirectX::Colors::White, 0.0f,
                          DirectX::SimpleMath::Vector2::Zero, 1.0f, DirectX::SpriteEffects_None, 0.0f);

        if (viewData->inspectorVisible.load() && viewData->inspectorTextureView &&
            viewData->inspectorTextureWidth > 0 && viewData->inspectorTextureHeight > 0) {
            DirectX::SimpleMath::Vector2 inspectorPosition(viewData->inspectorPosX, viewData->inspectorPosY);
            RECT inspectorSourceRect = {0, 0, static_cast<long>(viewData->inspectorTextureWidth),
                                        static_cast<long>(viewData->inspectorTextureHeight)};
            spriteBatch->Draw(viewData->inspectorTextureView, inspectorPosition, &inspectorSourceRect,
                              DirectX::Colors::White, 0.0f, DirectX::SimpleMath::Vector2::Zero, 1.0f,
                              DirectX::SpriteEffects_None, 0.0f);
        }
    }
}
