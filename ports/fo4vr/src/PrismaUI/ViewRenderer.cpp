#include "PCH.h"

#include "PrismaUI/ViewRenderer.h"

#include "PrismaUI/Core.h"
#include "PrismaUI/D3D11GpuDriver.h"
#include "PrismaUI/InputHandler.h"
#include "PrismaUI/SpatialPointer.h"
#include "PrismaUI/SpatialPresentation.h"
#include "PrismaUI/StereoProjection.h"
#include "PrismaUI/WorldPanelGeometry.h"

#include <DirectXTK/Effects.h>
#include <DirectXTK/PrimitiveBatch.h>
#include <DirectXTK/VertexTypes.h>

namespace PrismaUI::ViewRenderer
{
    namespace
    {
        struct ResolvedTarget
        {
            Core::GpuRenderTargetSnapshot snapshot{};
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;

            [[nodiscard]] bool IsValid() const noexcept
            {
                return snapshot.valid &&
                       texture &&
                       snapshot.width > 0 &&
                       snapshot.height > 0;
            }
        };

        struct DrawState
        {
            std::shared_ptr<Core::PrismaView> view;
            ResolvedTarget main;
            ResolvedTarget inspector;
            PRISMA_UI_VR_API::SpatialUpdateV1 spatial{};
            WorldPanelGeometry::WorldPanelSurface surface{};
            SpatialPointer::ReticleSnapshot reticle{};
            bool hasSpatial = false;
            bool world = false;
            bool hasSurface = false;
            bool backendReady = false;
            bool requestsDepth = false;
            int order = 0;
        };

        struct WorldResources
        {
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            std::unique_ptr<DirectX::BasicEffect> effect;
            std::unique_ptr<DirectX::PrimitiveBatch<
                DirectX::VertexPositionColorTexture>>
                batch;
            Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
            std::array<
                Microsoft::WRL::ComPtr<
                    ID3D11DepthStencilState>,
                9>
                depthReadStates{};
            std::array<bool, 9> depthStateFailed{};
        };

        [[nodiscard]] WorldResources& Resources() noexcept
        {
            // The container itself is process-lifetime so its destructor can
            // never run after FO4VR destroys the D3D device. Core explicitly
            // releases every contained device resource before retirement.
            static auto* resources = new WorldResources();
            return *resources;
        }

        [[nodiscard]] float Width(const RECT& rectangle) noexcept
        {
            return static_cast<float>(
                rectangle.right - rectangle.left);
        }

        [[nodiscard]] float Height(const RECT& rectangle) noexcept
        {
            return static_cast<float>(
                rectangle.bottom - rectangle.top);
        }

        [[nodiscard]] RECT ChildRectangle(
            const RECT& parent,
            float x,
            float y,
            float width,
            float height) noexcept
        {
            const auto& runtime = Core::GetRuntime();
            const auto scaleX =
                runtime.screenSize.width > 0 ?
                    Width(parent) /
                        runtime.screenSize.width :
                    1.0f;
            const auto scaleY =
                runtime.screenSize.height > 0 ?
                    Height(parent) /
                        runtime.screenSize.height :
                    1.0f;
            const auto left = static_cast<LONG>(std::lround(
                parent.left + x * scaleX));
            const auto top = static_cast<LONG>(std::lround(
                parent.top + y * scaleY));
            return {
                left,
                top,
                static_cast<LONG>(std::lround(
                    left + width * scaleX)),
                static_cast<LONG>(std::lround(
                    top + height * scaleY))
            };
        }

        [[nodiscard]] bool EnsureRenderer() noexcept
        {
            auto& runtime = Core::GetRuntime();
            auto& resources = Resources();
            if (resources.effect &&
                resources.batch &&
                resources.inputLayout) {
                return resources.device == runtime.device &&
                       resources.context == runtime.context;
            }
            if (!runtime.device || !runtime.context) {
                return false;
            }

            try {
                resources.device = runtime.device;
                resources.context = runtime.context;
                resources.effect =
                    std::make_unique<DirectX::BasicEffect>(
                        runtime.device);
                resources.effect->SetTextureEnabled(true);
                resources.effect->SetVertexColorEnabled(true);
                resources.effect->SetLightingEnabled(false);

                const void* bytecode = nullptr;
                std::size_t bytecodeLength = 0;
                resources.effect->GetVertexShaderBytecode(
                    &bytecode,
                    &bytecodeLength);
                if (!bytecode || bytecodeLength == 0) {
                    ReleaseDeviceResources();
                    return false;
                }

                const auto result =
                    runtime.device->CreateInputLayout(
                        DirectX::VertexPositionColorTexture::
                            InputElements,
                        DirectX::VertexPositionColorTexture::
                            InputElementCount,
                        bytecode,
                        bytecodeLength,
                        resources.inputLayout.GetAddressOf());
                if (FAILED(result)) {
                    ReleaseDeviceResources();
                    return false;
                }
                resources.batch = std::make_unique<
                    DirectX::PrimitiveBatch<
                        DirectX::VertexPositionColorTexture>>(
                    runtime.context);
                return true;
            } catch (...) {
                ReleaseDeviceResources();
                return false;
            }
        }

        [[nodiscard]] ID3D11DepthStencilState* DepthReadState(
            D3D11_COMPARISON_FUNC comparison) noexcept
        {
            const auto index =
                static_cast<std::size_t>(comparison);
            auto& resources = Resources();
            if (!resources.device ||
                index >= resources.depthReadStates.size() ||
                comparison < D3D11_COMPARISON_NEVER ||
                comparison > D3D11_COMPARISON_ALWAYS) {
                return nullptr;
            }
            if (resources.depthReadStates[index]) {
                return resources.depthReadStates[index].Get();
            }
            if (resources.depthStateFailed[index]) {
                return nullptr;
            }

            D3D11_DEPTH_STENCIL_DESC description{};
            description.DepthEnable = TRUE;
            description.DepthWriteMask =
                D3D11_DEPTH_WRITE_MASK_ZERO;
            description.DepthFunc = comparison;
            description.StencilEnable = FALSE;
            if (FAILED(resources.device->
                    CreateDepthStencilState(
                        &description,
                        resources.depthReadStates[index].
                            GetAddressOf()))) {
                resources.depthStateFailed[index] = true;
                return nullptr;
            }
            return resources.depthReadStates[index].Get();
        }

        [[nodiscard]] Core::GpuRenderTargetSnapshot Snapshot(
            const ultralight::RenderTarget& target,
            std::uint64_t generation) noexcept
        {
            Core::GpuRenderTargetSnapshot result;
            result.generation = generation;
            const auto& uv = target.uv_coords;
            if (target.is_empty ||
                target.texture_id == 0 ||
                target.render_buffer_id == 0 ||
                target.width == 0 ||
                target.height == 0 ||
                target.texture_width == 0 ||
                target.texture_height == 0 ||
                target.width > target.texture_width ||
                target.height > target.texture_height ||
                target.texture_format !=
                    ultralight::BitmapFormat::
                        BGRA8_UNORM_SRGB ||
                !std::isfinite(uv.left) ||
                !std::isfinite(uv.top) ||
                !std::isfinite(uv.right) ||
                !std::isfinite(uv.bottom) ||
                uv.left < 0.0f ||
                uv.top < 0.0f ||
                uv.right > 1.0f ||
                uv.bottom > 1.0f ||
                uv.right <= uv.left ||
                uv.bottom <= uv.top) {
                return result;
            }

            result.valid = true;
            result.textureId = target.texture_id;
            result.renderBufferId = target.render_buffer_id;
            result.width = target.width;
            result.height = target.height;
            result.textureWidth = target.texture_width;
            result.textureHeight = target.texture_height;
            result.format = target.texture_format;
            result.u0 = uv.left;
            result.v0 = uv.top;
            result.u1 = uv.right;
            result.v1 = uv.bottom;
            return result;
        }

        [[nodiscard]] DirectX::VertexPositionColorTexture Vertex(
            float x,
            float y,
            float z,
            float u,
            float v,
            float opacity = 1.0f) noexcept
        {
            return {
                DirectX::XMFLOAT3{x, y, z},
                DirectX::XMFLOAT4{
                    opacity,
                    opacity,
                    opacity,
                    opacity
                },
                DirectX::XMFLOAT2{u, v}
            };
        }

        [[nodiscard]] DirectX::VertexPositionColorTexture Vertex(
            const WorldPanelGeometry::Vec3& point,
            float u,
            float v,
            float opacity = 1.0f) noexcept
        {
            return Vertex(
                point.x,
                point.y,
                point.z,
                u,
                v,
                opacity);
        }

        [[nodiscard]] bool ConfigureCommonPipeline() noexcept
        {
            auto& runtime = Core::GetRuntime();
            auto& resources = Resources();
            if (!runtime.context ||
                !runtime.commonStates ||
                !resources.inputLayout) {
                return false;
            }

            runtime.context->SetPredication(nullptr, FALSE);
            runtime.context->HSSetShader(nullptr, nullptr, 0);
            runtime.context->DSSetShader(nullptr, nullptr, 0);
            runtime.context->GSSetShader(nullptr, nullptr, 0);
            runtime.context->IASetInputLayout(
                resources.inputLayout.Get());
            runtime.context->OMSetBlendState(
                runtime.commonStates->AlphaBlend(),
                nullptr,
                0xFFFFFFFFu);
            runtime.context->RSSetState(
                runtime.commonStates->CullNone());
            auto* sampler =
                runtime.commonStates->LinearClamp();
            runtime.context->PSSetSamplers(
                0,
                1,
                &sampler);
            return true;
        }

        [[nodiscard]] bool DrawScreenQuad(
            const ResolvedTarget& target,
            const RECT& destination,
            std::uint32_t viewportWidth,
            std::uint32_t viewportHeight,
            float opacity = 1.0f) noexcept
        {
            if (!target.IsValid() ||
                destination.right <= destination.left ||
                destination.bottom <= destination.top ||
                viewportWidth == 0 ||
                viewportHeight == 0 ||
                !EnsureRenderer() ||
                !ConfigureCommonPipeline()) {
                return false;
            }

            opacity = std::clamp(opacity, 0.0f, 1.0f);
            const auto& uv = target.snapshot;
            const std::array<
                DirectX::VertexPositionColorTexture,
                4>
                vertices{
                    Vertex(
                        static_cast<float>(destination.left),
                        static_cast<float>(destination.top),
                        0.0f,
                        uv.u0,
                        uv.v0,
                        opacity),
                    Vertex(
                        static_cast<float>(destination.right),
                        static_cast<float>(destination.top),
                        0.0f,
                        uv.u1,
                        uv.v0,
                        opacity),
                    Vertex(
                        static_cast<float>(destination.right),
                        static_cast<float>(destination.bottom),
                        0.0f,
                        uv.u1,
                        uv.v1,
                        opacity),
                    Vertex(
                        static_cast<float>(destination.left),
                        static_cast<float>(destination.bottom),
                        0.0f,
                        uv.u0,
                        uv.v1,
                        opacity)
                };

            auto& runtime = Core::GetRuntime();
            auto& resources = Resources();
            D3D11_VIEWPORT viewport{};
            viewport.Width =
                static_cast<float>(viewportWidth);
            viewport.Height =
                static_cast<float>(viewportHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            runtime.context->RSSetViewports(1, &viewport);
            runtime.context->OMSetDepthStencilState(
                runtime.commonStates->DepthNone(),
                0);

            resources.effect->SetTexture(target.texture.Get());
            resources.effect->SetWorld(
                DirectX::XMMatrixIdentity());
            resources.effect->SetView(
                DirectX::XMMatrixIdentity());
            resources.effect->SetProjection(
                DirectX::XMMatrixOrthographicOffCenterLH(
                    0.0f,
                    static_cast<float>(viewportWidth),
                    static_cast<float>(viewportHeight),
                    0.0f,
                    0.0f,
                    1.0f));
            resources.effect->Apply(runtime.context);
            resources.batch->Begin();
            resources.batch->DrawQuad(
                vertices[0],
                vertices[1],
                vertices[2],
                vertices[3]);
            resources.batch->End();
            resources.effect->SetTexture(nullptr);
            return true;
        }

        [[nodiscard]] bool DrawWorldQuad(
            const ResolvedTarget& target,
            const std::array<
                DirectX::VertexPositionColorTexture,
                4>& vertices,
            const StereoProjection::Snapshot& projection,
            bool useDepth,
            const SceneDepthCapture::FrameDepth* sceneDepth) noexcept
        {
            auto& runtime = Core::GetRuntime();
            if (!target.IsValid() ||
                !runtime.stereoPanelLayout.valid ||
                !EnsureRenderer() ||
                !ConfigureCommonPipeline()) {
                return false;
            }

            ID3D11DepthStencilState* depthState =
                runtime.commonStates->DepthNone();
            if (useDepth) {
                if (!sceneDepth || !sceneDepth->IsValid()) {
                    return false;
                }
                depthState =
                    DepthReadState(sceneDepth->comparison);
                if (!depthState) {
                    return false;
                }
            }

            auto& resources = Resources();
            resources.effect->SetTexture(target.texture.Get());
            resources.effect->SetView(
                DirectX::XMMatrixIdentity());
            const std::array<RECT, 2> eyeRectangles{
                runtime.stereoPanelLayout.leftEyeRect,
                runtime.stereoPanelLayout.rightEyeRect
            };
            for (std::size_t eye = 0;
                 eye < eyeRectangles.size();
                 ++eye) {
                const auto& rectangle = eyeRectangles[eye];
                if (rectangle.right <= rectangle.left ||
                    rectangle.bottom <= rectangle.top) {
                    resources.effect->SetTexture(nullptr);
                    return false;
                }
                D3D11_VIEWPORT viewport{};
                viewport.TopLeftX =
                    static_cast<float>(rectangle.left);
                viewport.TopLeftY =
                    static_cast<float>(rectangle.top);
                viewport.Width = Width(rectangle);
                viewport.Height = Height(rectangle);
                viewport.MinDepth = 0.0f;
                viewport.MaxDepth = 1.0f;
                runtime.context->RSSetViewports(1, &viewport);
                runtime.context->OMSetDepthStencilState(
                    depthState,
                    0);

                const auto& origin = projection.origin[eye];
                resources.effect->SetWorld(
                    DirectX::XMMatrixTranslation(
                        -origin.x,
                        -origin.y,
                        -origin.z));
                resources.effect->SetProjection(
                    DirectX::XMLoadFloat4x4(
                        &projection.composite[eye]));
                resources.effect->Apply(runtime.context);
                resources.batch->Begin();
                resources.batch->DrawQuad(
                    vertices[0],
                    vertices[1],
                    vertices[2],
                    vertices[3]);
                resources.batch->End();
            }
            resources.effect->SetTexture(nullptr);
            return true;
        }

        [[nodiscard]] bool ResolveTarget(
            DrawState& state) noexcept
        {
            auto& runtime = Core::GetRuntime();
            if (!runtime.gpuDriver ||
                runtime.gpuDriver->HasFatalError() ||
                !state.view) {
                return false;
            }
            {
                std::lock_guard lock(
                    state.view->renderTargetMutex);
                state.main.snapshot =
                    state.view->renderTarget;
                state.inspector.snapshot =
                    state.view->inspectorRenderTarget;
            }
            if (!state.main.snapshot.valid) {
                return false;
            }
            state.main.texture =
                runtime.gpuDriver->GetTextureView(
                    state.main.snapshot.textureId);
            if (!state.main.texture) {
                return false;
            }
            if (state.inspector.snapshot.valid) {
                state.inspector.texture =
                    runtime.gpuDriver->GetTextureView(
                        state.inspector.snapshot.textureId);
            }
            return true;
        }

        [[nodiscard]] bool DrawHeadLocked(
            const ResolvedTarget& target,
            RenderLayout layout) noexcept
        {
            auto& runtime = Core::GetRuntime();
            if (!target.IsValid()) {
                return false;
            }
            if (layout == RenderLayout::HeadLockedStereo &&
                runtime.stereoPanelLayout.valid) {
                const auto left = DrawScreenQuad(
                    target,
                    runtime.stereoPanelLayout.leftRect,
                    runtime.stereoPanelLayout.atlasWidth,
                    runtime.stereoPanelLayout.atlasHeight);
                const auto right = DrawScreenQuad(
                    target,
                    runtime.stereoPanelLayout.rightRect,
                    runtime.stereoPanelLayout.atlasWidth,
                    runtime.stereoPanelLayout.atlasHeight);
                return left && right;
            }

            const auto viewportWidth =
                layout == RenderLayout::SideBySideStereo ?
                    runtime.screenSize.width * 2u :
                    runtime.screenSize.width;
            RECT destination{
                0,
                0,
                static_cast<LONG>(runtime.screenSize.width),
                static_cast<LONG>(runtime.screenSize.height)
            };
            auto result = DrawScreenQuad(
                target,
                destination,
                viewportWidth,
                runtime.screenSize.height);
            if (layout == RenderLayout::SideBySideStereo) {
                destination.left +=
                    runtime.screenSize.width;
                destination.right +=
                    runtime.screenSize.width;
                result =
                    DrawScreenQuad(
                        target,
                        destination,
                        viewportWidth,
                        runtime.screenSize.height) &&
                    result;
            }
            return result;
        }

        void DrawInspector(
            const DrawState& state,
            RenderLayout layout) noexcept
        {
            if (!state.view ||
                !state.inspector.IsValid() ||
                !state.view->inspectorVisible.load(
                    std::memory_order_acquire)) {
                return;
            }

            Core::InspectorPresentationState presentation;
            {
                std::lock_guard lock(
                    state.view->inspectorPresentationMutex);
                presentation =
                    state.view->inspectorPresentation;
            }
            const auto width =
                presentation.width > 0 ?
                    static_cast<float>(presentation.width) :
                    static_cast<float>(
                        state.inspector.snapshot.width);
            const auto height =
                presentation.height > 0 ?
                    static_cast<float>(presentation.height) :
                    static_cast<float>(
                        state.inspector.snapshot.height);
            auto& runtime = Core::GetRuntime();
            if (layout == RenderLayout::HeadLockedStereo &&
                runtime.stereoPanelLayout.valid) {
                (void)DrawScreenQuad(
                    state.inspector,
                    ChildRectangle(
                        runtime.stereoPanelLayout.leftRect,
                        presentation.x,
                        presentation.y,
                        width,
                        height),
                    runtime.stereoPanelLayout.atlasWidth,
                    runtime.stereoPanelLayout.atlasHeight,
                    presentation.opacity);
                (void)DrawScreenQuad(
                    state.inspector,
                    ChildRectangle(
                        runtime.stereoPanelLayout.rightRect,
                        presentation.x,
                        presentation.y,
                        width,
                        height),
                    runtime.stereoPanelLayout.atlasWidth,
                    runtime.stereoPanelLayout.atlasHeight,
                    presentation.opacity);
                return;
            }

            RECT destination{
                static_cast<LONG>(std::lround(presentation.x)),
                static_cast<LONG>(std::lround(presentation.y)),
                static_cast<LONG>(std::lround(
                    presentation.x + width)),
                static_cast<LONG>(std::lround(
                    presentation.y + height))
            };
            const auto viewportWidth =
                layout == RenderLayout::SideBySideStereo ?
                    runtime.screenSize.width * 2u :
                    runtime.screenSize.width;
            (void)DrawScreenQuad(
                state.inspector,
                destination,
                viewportWidth,
                runtime.screenSize.height,
                presentation.opacity);
            if (layout == RenderLayout::SideBySideStereo) {
                destination.left += runtime.screenSize.width;
                destination.right += runtime.screenSize.width;
                (void)DrawScreenQuad(
                    state.inspector,
                    destination,
                    viewportWidth,
                    runtime.screenSize.height,
                    presentation.opacity);
            }
        }

        [[nodiscard]] bool BuildWorldVertices(
            DrawState& state,
            const StereoProjection::Snapshot& projection,
            std::array<
                DirectX::VertexPositionColorTexture,
                4>& vertices) noexcept
        {
            WorldPanelGeometry::WorldPanelPlacement placement;
            if (!WorldPanelGeometry::MakePlacement(
                    state.spatial,
                    placement)) {
                return false;
            }
            const WorldPanelGeometry::Vec3 midpoint{
                (projection.origin[0].x +
                 projection.origin[1].x) *
                    0.5f,
                (projection.origin[0].y +
                 projection.origin[1].y) *
                    0.5f,
                (projection.origin[0].z +
                 projection.origin[1].z) *
                    0.5f
            };
            if (!WorldPanelGeometry::ResolveSurface(
                    placement,
                    midpoint,
                    state.surface)) {
                return false;
            }
            const auto& corners = state.surface.corners;
            const auto& uv = state.main.snapshot;
            vertices = {
                Vertex(corners[0], uv.u0, uv.v0),
                Vertex(corners[1], uv.u1, uv.v0),
                Vertex(corners[2], uv.u1, uv.v1),
                Vertex(corners[3], uv.u0, uv.v1)
            };
            state.hasSurface = true;
            return true;
        }

        [[nodiscard]] bool DrawReticle(
            const SpatialPointer::ReticleSnapshot& reticle,
            const StereoProjection::Snapshot& projection,
            bool useDepth,
            const SceneDepthCapture::FrameDepth* sceneDepth) noexcept
        {
            auto& runtime = Core::GetRuntime();
            if (!reticle.visible ||
                !runtime.cursorTexture ||
                reticle.physicalWidth <= 0.0f ||
                reticle.physicalHeight <= 0.0f) {
                return false;
            }

            const auto cross = [](const auto& left, const auto& right) {
                return WorldPanelGeometry::Vec3{
                    left.y * right.z - left.z * right.y,
                    left.z * right.x - left.x * right.z,
                    left.x * right.y - left.y * right.x
                };
            };
            const auto normal = cross(reticle.right, reticle.up);
            const auto halfRight = reticle.physicalWidth * 0.5f;
            const auto halfUp = reticle.physicalHeight * 0.5f;
            const auto point = [&](
                                   float right,
                                   float up) {
                return WorldPanelGeometry::Vec3{
                    reticle.center.x +
                        reticle.right.x * right +
                        reticle.up.x * up +
                        normal.x * 0.02f,
                    reticle.center.y +
                        reticle.right.y * right +
                        reticle.up.y * up +
                        normal.y * 0.02f,
                    reticle.center.z +
                        reticle.right.z * right +
                        reticle.up.z * up +
                        normal.z * 0.02f
                };
            };
            const std::array<
                DirectX::VertexPositionColorTexture,
                4>
                vertices{
                    Vertex(point(-halfRight, halfUp), 0.0f, 0.0f),
                    Vertex(point(halfRight, halfUp), 1.0f, 0.0f),
                    Vertex(point(halfRight, -halfUp), 1.0f, 1.0f),
                    Vertex(point(-halfRight, -halfUp), 0.0f, 1.0f)
                };
            ResolvedTarget target;
            target.snapshot.valid = true;
            target.snapshot.width = 16;
            target.snapshot.height = 24;
            target.snapshot.u1 = 1.0f;
            target.snapshot.v1 = 1.0f;
            target.texture = runtime.cursorTexture;
            return DrawWorldQuad(
                target,
                vertices,
                projection,
                useDepth,
                sceneDepth);
        }
    }

    void PublishRenderTargets() noexcept
    {
        auto& runtime = Core::GetRuntime();
        if (!runtime.worker.IsWorkerThread()) {
            return;
        }

        std::vector<std::shared_ptr<Core::PrismaView>> views;
        {
            std::shared_lock lock(runtime.viewsMutex);
            views.reserve(runtime.views.size());
            for (const auto& [id, view] : runtime.views) {
                (void)id;
                if (view &&
                    !view->destroying.load(
                        std::memory_order_acquire)) {
                    views.push_back(view);
                }
            }
        }

        for (const auto& view : views) {
            Core::GpuRenderTargetSnapshot main;
            Core::GpuRenderTargetSnapshot inspector;
            {
                std::lock_guard lock(view->renderTargetMutex);
                main.generation =
                    (std::max)(
                        view->renderTarget.generation,
                        view->pendingRenderTarget.generation) +
                    1;
                inspector.generation =
                    (std::max)(
                        view->inspectorRenderTarget.generation,
                        view->pendingInspectorRenderTarget.generation) +
                    1;
            }
            try {
                if (view->ultralightView &&
                    view->ultralightView->is_accelerated()) {
                    main = Snapshot(
                        view->ultralightView->render_target(),
                        main.generation);
                }
                if (view->inspectorView &&
                    view->inspectorView->is_accelerated()) {
                    inspector = Snapshot(
                        view->inspectorView->render_target(),
                        inspector.generation);
                }
            } catch (...) {
            }
            std::lock_guard lock(view->renderTargetMutex);
            view->pendingRenderTarget = main;
            view->pendingInspectorRenderTarget = inspector;
        }
    }

    void CommitPublishedRenderTargets() noexcept
    {
        auto& runtime = Core::GetRuntime();
        std::shared_lock lock(runtime.viewsMutex);
        for (const auto& [id, view] : runtime.views) {
            (void)id;
            if (!view) {
                continue;
            }
            std::lock_guard targetLock(view->renderTargetMutex);
            view->renderTarget = view->pendingRenderTarget;
            view->inspectorRenderTarget =
                view->pendingInspectorRenderTarget;
        }
    }

    void DrawViews(
        RenderLayout layout,
        const SceneDepthCapture::FrameDepth* sceneDepth) noexcept
    {
        auto& runtime = Core::GetRuntime();
        if (!runtime.device ||
            !runtime.context ||
            !runtime.commonStates ||
            !runtime.gpuDriver ||
            runtime.gpuDriver->HasFatalError()) {
            SpatialPointer::HandleBackendUnavailable();
            return;
        }

        thread_local std::vector<DrawState> states;
        states.clear();
        {
            std::shared_lock lock(runtime.viewsMutex);
            if (states.capacity() < runtime.views.size()) {
                states.reserve(runtime.views.size());
            }
            for (const auto& [id, view] : runtime.views) {
                (void)id;
                if (!view ||
                    view->hidden.load(
                        std::memory_order_acquire) ||
                    view->destroying.load(
                        std::memory_order_acquire)) {
                    continue;
                }
                DrawState state;
                state.view = view;
                state.order = view->order.load(
                    std::memory_order_acquire);
                state.hasSpatial =
                    SpatialPresentation::BeginFrame(
                        view,
                        state.spatial);
                state.world =
                    state.hasSpatial &&
                    (state.spatial.presentationMode ==
                         PRISMA_UI_VR_API::
                             SpatialPresentationMode::
                                 WorldBillboard ||
                     state.spatial.presentationMode ==
                         PRISMA_UI_VR_API::
                             SpatialPresentationMode::
                                 WorldQuad);
                state.requestsDepth =
                    state.world &&
                    (state.spatial.flags &
                     PRISMA_UI_VR_API::
                         SpatialUpdate_SceneDepthOcclusion) != 0;
                states.push_back(std::move(state));
            }
        }
        if (states.empty()) {
            SceneDepthCapture::SetCaptureRequested(false);
            return;
        }

        const auto needsDepth = std::any_of(
            states.begin(),
            states.end(),
            [](const DrawState& state) {
                return state.requestsDepth;
            });
        SceneDepthCapture::SetCaptureRequested(needsDepth);

        std::erase_if(
            states,
            [](DrawState& state) {
                if (ResolveTarget(state)) {
                    return false;
                }
                SpatialPresentation::MarkBackendUnavailable(
                    state.view);
                return true;
            });
        if (states.empty()) {
            SpatialPointer::HandleBackendUnavailable();
            return;
        }
        std::sort(
            states.begin(),
            states.end(),
            [](const DrawState& left, const DrawState& right) {
                if (left.order != right.order) {
                    return left.order < right.order;
                }
                return left.view->id < right.view->id;
            });

        const auto needsProjection = std::any_of(
            states.begin(),
            states.end(),
            [](const DrawState& state) {
                return state.world;
            });
        StereoProjection::Snapshot projection;
        const auto projectionReady =
            !needsProjection ||
            (layout == RenderLayout::HeadLockedStereo &&
             StereoProjection::CaptureSnapshot(projection));

        try {
            for (auto& state : states) {
                if (state.world) {
                    std::array<
                        DirectX::VertexPositionColorTexture,
                        4>
                        vertices{};
                    if (projectionReady &&
                        BuildWorldVertices(
                            state,
                            projection,
                            vertices)) {
                        state.backendReady = DrawWorldQuad(
                            state.main,
                            vertices,
                            projection,
                            state.requestsDepth,
                            sceneDepth);
                    }
                } else {
                    state.backendReady =
                        DrawHeadLocked(state.main, layout);
                }
                DrawInspector(state, layout);
                if (state.hasSpatial) {
                    SpatialPresentation::MarkApplied(
                        state.view,
                        state.spatial,
                        state.backendReady,
                        state.main.snapshot.width,
                        state.main.snapshot.height);
                }
            }

            thread_local std::vector<
                SpatialPointer::FrameTarget>
                pointerTargets;
            pointerTargets.clear();
            if (pointerTargets.capacity() < states.size()) {
                pointerTargets.reserve(states.size());
            }
            for (auto& state : states) {
                pointerTargets.push_back({
                    .view = state.view.get(),
                    .surface =
                        state.hasSurface ?
                            &state.surface :
                            nullptr,
                    .backendReady = state.backendReady,
                    .drawOrder = state.order
                });
            }
            SpatialPointer::ProcessFrameBatch(pointerTargets);
            if (projectionReady) {
                for (std::size_t index = 0;
                     index < states.size();
                     ++index) {
                    states[index].reticle =
                        pointerTargets[index].reticle;
                    if (states[index].reticle.visible) {
                        (void)DrawReticle(
                            states[index].reticle,
                            projection,
                            states[index].requestsDepth,
                            sceneDepth);
                    }
                }
            }
        } catch (...) {
            logger::error(
                "PrismaUI view composition failed");
            for (const auto& state : states) {
                SpatialPresentation::MarkBackendUnavailable(
                    state.view);
            }
            SpatialPointer::HandleBackendUnavailable();
        }
        states.clear();
    }

    void DrawCursor(RenderLayout layout) noexcept
    {
        auto& runtime = Core::GetRuntime();
        if (!runtime.cursorTexture ||
            !InputHandler::IsAnyInputCaptureActive()) {
            return;
        }

        ResolvedTarget cursor;
        cursor.snapshot.valid = true;
        cursor.snapshot.width = 16;
        cursor.snapshot.height = 24;
        cursor.snapshot.u1 = 1.0f;
        cursor.snapshot.v1 = 1.0f;
        cursor.texture = runtime.cursorTexture;
        const auto position =
            InputHandler::GetLastCursorPosition();

        if (layout == RenderLayout::HeadLockedStereo &&
            runtime.stereoPanelLayout.valid) {
            const auto drawForEye = [&](
                                        const RECT& eye) {
                const auto scaleX =
                    runtime.screenSize.width > 0 ?
                        Width(eye) /
                            runtime.screenSize.width :
                        1.0f;
                const auto scaleY =
                    runtime.screenSize.height > 0 ?
                        Height(eye) /
                            runtime.screenSize.height :
                        1.0f;
                const auto left =
                    static_cast<LONG>(std::lround(
                        eye.left +
                        position.x * scaleX));
                const auto top =
                    static_cast<LONG>(std::lround(
                        eye.top +
                        position.y * scaleY));
                const RECT destination{
                    left,
                    top,
                    static_cast<LONG>(std::lround(
                        left + 16.0f * scaleX)),
                    static_cast<LONG>(std::lround(
                        top + 24.0f * scaleY))
                };
                (void)DrawScreenQuad(
                    cursor,
                    destination,
                    runtime.stereoPanelLayout.atlasWidth,
                    runtime.stereoPanelLayout.atlasHeight);
            };
            drawForEye(runtime.stereoPanelLayout.leftRect);
            drawForEye(runtime.stereoPanelLayout.rightRect);
            return;
        }

        RECT destination{
            position.x,
            position.y,
            position.x + 16,
            position.y + 24
        };
        const auto viewportWidth =
            layout == RenderLayout::SideBySideStereo ?
                runtime.screenSize.width * 2u :
                runtime.screenSize.width;
        (void)DrawScreenQuad(
            cursor,
            destination,
            viewportWidth,
            runtime.screenSize.height);
        if (layout == RenderLayout::SideBySideStereo) {
            destination.left += runtime.screenSize.width;
            destination.right += runtime.screenSize.width;
            (void)DrawScreenQuad(
                cursor,
                destination,
                viewportWidth,
                runtime.screenSize.height);
        }
    }

    void ReleaseDeviceResources() noexcept
    {
        auto& resources = Resources();
        resources.batch.reset();
        resources.effect.reset();
        resources.inputLayout.Reset();
        for (auto& state : resources.depthReadStates) {
            state.Reset();
        }
        resources.depthStateFailed.fill(false);
        resources.device = nullptr;
        resources.context = nullptr;
    }
}
