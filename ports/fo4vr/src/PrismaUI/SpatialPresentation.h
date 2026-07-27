#pragma once

#include "PrismaUI/Core.h"

#include <cstdint>
#include <utility>

namespace PrismaUI::SpatialPresentation
{
    inline constexpr std::uint32_t kMaximumPixelDimension = 4096;
    inline constexpr std::uint32_t kMaximumSpatialViews = 16;
    inline constexpr std::uint64_t kMaximumAggregateSpatialPixels =
        16ull * 1024ull * 1024ull;

    [[nodiscard]] PRISMA_UI_VR_API::SpatialResult GetCapabilities(
        PRISMA_UI_VR_API::SpatialCapabilitiesV1* outCapabilities) noexcept;
    [[nodiscard]] PRISMA_UI_VR_API::SpatialResult SubmitUpdate(
        Core::PrismaViewId viewId,
        const PRISMA_UI_VR_API::SpatialUpdateV1* update) noexcept;
    [[nodiscard]] PRISMA_UI_VR_API::SpatialResult GetState(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::SpatialStateV1* outState) noexcept;

    [[nodiscard]] bool BeginFrame(
        const std::shared_ptr<Core::PrismaView>& view,
        PRISMA_UI_VR_API::SpatialUpdateV1& outActive) noexcept;
    [[nodiscard]] bool HasRenderOnlyWorldPresentation(
        const std::shared_ptr<Core::PrismaView>& view) noexcept;
    [[nodiscard]] bool IsTransitioningToHeadLocked(
        const std::shared_ptr<Core::PrismaView>& view) noexcept;
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
        GetDesiredPixelSize(
            const std::shared_ptr<Core::PrismaView>& view,
            std::uint32_t fallbackWidth,
            std::uint32_t fallbackHeight) noexcept;

    void MarkApplied(
        const std::shared_ptr<Core::PrismaView>& view,
        const PRISMA_UI_VR_API::SpatialUpdateV1& frameUpdate,
        bool backendReady,
        std::uint32_t renderedPixelWidth,
        std::uint32_t renderedPixelHeight) noexcept;
    void MarkBackendUnavailable(
        const std::shared_ptr<Core::PrismaView>& view) noexcept;
}
