#pragma once

#include "PrismaUI/Core.h"
#include "PrismaUI/WorldPanelGeometry.h"

#include <memory>
#include <span>

namespace PrismaUI::SpatialPointer
{
    struct ReticleSnapshot
    {
        bool visible = false;
        WorldPanelGeometry::Vec3 center{};
        WorldPanelGeometry::Vec3 right{};
        WorldPanelGeometry::Vec3 up{};
        float physicalWidth = 0.0f;
        float physicalHeight = 0.0f;
    };

    struct FrameTarget
    {
        // Non-owning for the duration of ProcessFrameBatch. The renderer
        // retains each corresponding shared_ptr around this call.
        Core::PrismaView* view = nullptr;
        const WorldPanelGeometry::WorldPanelSurface* surface = nullptr;
        bool backendReady = false;
        int drawOrder = 0;
        ReticleSnapshot reticle{};
    };

    [[nodiscard]] PRISMA_UI_VR_API::SpatialResult SubmitUpdate(
        Core::PrismaViewId viewId,
        const PRISMA_UI_VR_API::SpatialPointerUpdateV1* update) noexcept;
    [[nodiscard]] PRISMA_UI_VR_API::SpatialResult Cancel(
        Core::PrismaViewId viewId) noexcept;
    [[nodiscard]] PRISMA_UI_VR_API::SpatialResult GetState(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::SpatialPointerStateV1* outState) noexcept;

    void ProcessFrameBatch(std::span<FrameTarget> targets) noexcept;
    [[nodiscard]] bool CancelForView(
        const std::shared_ptr<Core::PrismaView>& view,
        bool dispatchEvents) noexcept;
    void HandleBackendUnavailable() noexcept;
    void Shutdown() noexcept;
}
