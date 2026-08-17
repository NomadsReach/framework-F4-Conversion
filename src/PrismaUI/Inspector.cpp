#include "Inspector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "Core.h"
#include "Utils/DllLoader.h"
#include "ViewManager.h"

#ifdef min
    #undef min
#endif
#ifdef max
    #undef max
#endif

namespace PrismaUI::Inspector {
    constexpr uint32_t kBytesPerPixel = 4;
    using namespace Core;

    namespace {
        std::once_flag inspectorAssetCheckFlag;
        std::atomic<bool> inspectorAssetsAvailable{false};

        std::shared_ptr<PrismaView> FindLiveView(PrismaViewId viewId) {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it == views.end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) {
                return nullptr;
            }
            return it->second;
        }
    }

    void EnsureInspectorAssetsAvailability() {
        const auto inspectorPath = Utils::GetBasePath() / "inspector" / "Main.html";
        std::call_once(inspectorAssetCheckFlag, [inspectorPath]() {
            try {
                if (std::filesystem::exists(inspectorPath)) {
                    inspectorAssetsAvailable.store(true);
                    logger::info("Ultralight inspector assets detected at {}", inspectorPath.string());
                } else {
                    logger::warn(
                        "Ultralight inspector assets were not found at {}. Inspector view will not render unless the "
                        "SDK inspector folder is copied next to the DLL.",
                        inspectorPath.string());
                }
            } catch (const std::exception& e) {
                logger::warn("Failed to verify Ultralight inspector asset directory at {}: {}", inspectorPath.string(),
                             e.what());
            }
        });
    }

    bool AreInspectorAssetsAvailable() {
        EnsureInspectorAssetsAvailability();
        return inspectorAssetsAvailable.load();
    }

    void ReleaseInspectorTexture(PrismaView* viewData) {
        if (!viewData) {
            return;
        }

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

    void DestroyInspectorResources(PrismaView* viewData) {
        if (!viewData) {
            return;
        }

        ReleaseInspectorTexture(viewData);
        {
            std::lock_guard bufferLock(viewData->inspectorBufferMutex);
            viewData->inspectorPixelBuffer.clear();
            viewData->inspectorPixelBuffer.shrink_to_fit();
            viewData->inspectorBufferWidth = 0;
            viewData->inspectorBufferHeight = 0;
            viewData->inspectorBufferStride = 0;
        }

        viewData->inspectorFrameReady.store(false);
        viewData->inspectorPointerHover.store(false);
    }

    void CreateInspectorView(const PrismaViewId& viewId) {
        if (!AreInspectorAssetsAvailable()) {
            logger::warn(
                "View [{}]: Inspector assets were not found. Copy the Ultralight inspector folder next to PrismaUI.dll "
                "to enable the inspector.",
                viewId);
            return;
        }

        auto viewData = FindLiveView(viewId);
        if (!viewData) {
            logger::warn("CreateInspectorView: View ID [{}] not found.", viewId);
            return;
        }
        if (viewData->inspectorView) {
            logger::info("View [{}]: Inspector view already exists.", viewId);
            return;
        }
        if (!viewData->ultralightView) {
            logger::warn("View [{}]: Cannot create inspector because Ultralight view is not ready yet.", viewId);
            return;
        }

        try {
            auto createInspector = [view = viewData]() {
                if (!view->isDestroying.load(std::memory_order_acquire) && view->ultralightView) {
                    view->ultralightView->CreateLocalInspectorView();
                }
            };

            if (ultralightThread.IsWorkerThread()) {
                createInspector();
            } else {
                ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::MEDIUM, createInspector).get();
            }
            logger::info("View [{}]: Inspector creation requested.", viewId);
        } catch (const std::exception& e) {
            logger::error("View [{}]: Exception while creating inspector view: {}", viewId, e.what());
        }
    }

    void SetInspectorVisibility(const PrismaViewId& viewId, bool visible) {
        auto viewData = FindLiveView(viewId);
        if (!viewData) {
            logger::warn("SetInspectorVisibility: View ID [{}] not found.", viewId);
            return;
        }

        if (!viewData->inspectorView && visible) {
            CreateInspectorView(viewId);
        }
        if (viewData->isDestroying.load(std::memory_order_acquire) || !viewData->inspectorView) {
            logger::warn("View [{}]: Inspector view is not available to {}.", viewId, visible ? "show" : "hide");
            return;
        }

        viewData->inspectorVisible.store(visible);
        viewData->inspectorPointerHover.store(false);

        if (visible) {
            auto focusInspector = [view = viewData]() {
                if (view->isDestroying.load(std::memory_order_acquire)) {
                    return;
                }
                if (view->inspectorView) {
                    view->inspectorView->Focus();
                }
                if (view->ultralightView && view->ultralightView->HasFocus()) {
                    view->ultralightView->Unfocus();
                }
            };

            if (ultralightThread.IsWorkerThread()) {
                focusInspector();
            } else {
                ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::MEDIUM, focusInspector).wait();
            }
        }

        logger::info("View [{}]: Inspector visibility set to {}.", viewId, visible);
    }

    bool IsInspectorVisible(const PrismaViewId& viewId) {
        auto viewData = FindLiveView(viewId);
        return viewData && viewData->inspectorVisible.load();
    }

    void SetInspectorBounds(const PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width,
                            uint32_t height) {
        width = std::max<uint32_t>(width, 32u);
        height = std::max<uint32_t>(height, 32u);

        auto viewData = FindLiveView(viewId);
        if (!viewData) {
            logger::warn("SetInspectorBounds: View ID [{}] not found.", viewId);
            return;
        }
        if (!viewData->inspectorView) {
            logger::warn("View [{}]: Cannot set inspector bounds because inspector is not available.", viewId);
            return;
        }

        const float screenWidth = static_cast<float>(screenSize.width ? screenSize.width : width);
        const float screenHeight = static_cast<float>(screenSize.height ? screenSize.height : height);
        const float maxX = std::max(0.0f, screenWidth - static_cast<float>(width));
        const float maxY = std::max(0.0f, screenHeight - static_cast<float>(height));

        viewData->inspectorPosX = std::clamp(topLeftX, 0.0f, maxX);
        viewData->inspectorPosY = std::clamp(topLeftY, 0.0f, maxY);
        viewData->inspectorDisplayWidth = width;
        viewData->inspectorDisplayHeight = height;
        viewData->inspectorPointerHover.store(false);

        try {
            auto resizeInspector = [view = viewData, width, height]() {
                if (!view->isDestroying.load(std::memory_order_acquire) && view->inspectorView) {
                    view->inspectorView->Resize(width, height);
                }
            };

            if (ultralightThread.IsWorkerThread()) {
                resizeInspector();
            } else {
                ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::MEDIUM, resizeInspector).wait();
            }
            logger::info("View [{}]: Inspector bounds set to ({}, {}) with size {}x{}", viewId, topLeftX, topLeftY,
                         width, height);
        } catch (const std::exception& e) {
            logger::error("View [{}]: Exception while setting inspector bounds: {}", viewId, e.what());
        }
    }

    void RenderInspectorView(std::shared_ptr<PrismaView> viewData) {
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->inspectorView ||
            !viewData->inspectorVisible.load() || viewData->isHidden.load()) {
            return;
        }

        if (viewData->inspectorView->surface()) {
            CopyInspectorBitmapToBuffer(viewData);
        }
    }

    void CopyInspectorBitmapToBuffer(std::shared_ptr<PrismaView> viewData) {
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->inspectorView) {
            return;
        }

        Surface* surface = viewData->inspectorView->surface();
        if (!surface) {
            return;
        }

        // The default BitmapSurfaceFactory guarantees a BitmapSurface here.
        auto* bitmapSurface = static_cast<BitmapSurface*>(surface);
        RefPtr<Bitmap> bitmap = bitmapSurface->bitmap();
        if (!bitmap || bitmap->IsEmpty()) {
            return;
        }

        const void* pixels = bitmap->LockPixels();
        if (!pixels) {
            return;
        }

        const uint32_t width = bitmap->width();
        const uint32_t height = bitmap->height();
        const uint32_t stride = bitmap->row_bytes();
        const size_t dataSize = stride * height;

        {
            std::lock_guard<std::mutex> lock(viewData->inspectorBufferMutex);
            if (viewData->inspectorPixelBuffer.size() != dataSize) {
                viewData->inspectorPixelBuffer.resize(dataSize);
            }
            std::memcpy(viewData->inspectorPixelBuffer.data(), pixels, dataSize);
            viewData->inspectorBufferWidth = width;
            viewData->inspectorBufferHeight = height;
            viewData->inspectorBufferStride = stride;
            viewData->inspectorFrameReady.store(true);
        }

        bitmap->UnlockPixels();
    }

    void CopyInspectorPixelsToTexture(PrismaView* viewData, void* pixels, uint32_t width, uint32_t height,
                                      uint32_t stride) {
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !pixels || !d3dContext ||
            !d3dDevice || width == 0 || height == 0) {
            return;
        }

        if (!viewData->inspectorTexture || viewData->inspectorTextureWidth != width ||
            viewData->inspectorTextureHeight != height) {
            ReleaseInspectorTexture(viewData);

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

            HRESULT result = d3dDevice->CreateTexture2D(&textureDesc, nullptr, &viewData->inspectorTexture);
            if (FAILED(result)) {
                logger::error("Failed to create inspector D3D11 texture for View [{}]: HRESULT={:X}", viewData->id,
                              static_cast<unsigned>(result));
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
            viewDesc.Format = textureDesc.Format;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;

            result = d3dDevice->CreateShaderResourceView(viewData->inspectorTexture, &viewDesc,
                                                         &viewData->inspectorTextureView);
            if (FAILED(result)) {
                logger::error("Failed to create inspector shader resource view for View [{}]: HRESULT={:X}",
                              viewData->id, static_cast<unsigned>(result));
                viewData->inspectorTexture->Release();
                viewData->inspectorTexture = nullptr;
                return;
            }

            viewData->inspectorTextureWidth = width;
            viewData->inspectorTextureHeight = height;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result = d3dContext->Map(viewData->inspectorTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result)) {
            logger::error("Failed to map inspector texture for View [{}]: HRESULT={:X}", viewData->id,
                          static_cast<unsigned>(result));
            return;
        }

        const uint8_t* source = static_cast<const uint8_t*>(pixels);
        auto* destination = static_cast<uint8_t*>(mapped.pData);
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(destination + row * mapped.RowPitch, source + row * stride, width * kBytesPerPixel);
        }
        d3dContext->Unmap(viewData->inspectorTexture, 0);
    }
}
