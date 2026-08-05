#pragma once

#include "PrismaUI_F4VR_API.h"

#include <array>
#include <cstdint>

namespace PrismaUI::WorldPanelGeometry
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct WorldPanelPlacement
    {
        PRISMA_UI_VR_API::SpatialPresentationMode mode =
            PRISMA_UI_VR_API::SpatialPresentationMode::HeadLockedQuad;
        Vec3 center{};
        std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t pixelWidth = 0;
        std::uint32_t pixelHeight = 0;
        float physicalWidth = 0.0f;
        float physicalHeight = 0.0f;
    };

    struct WorldPanelSurface
    {
        Vec3 center{};
        Vec3 right{};
        Vec3 up{};
        Vec3 normal{};
        float physicalWidth = 0.0f;
        float physicalHeight = 0.0f;
        std::uint32_t pixelWidth = 0;
        std::uint32_t pixelHeight = 0;
        std::array<Vec3, 4> corners{};
    };

    struct WorldPanelRayHit
    {
        Vec3 position{};
        float distance = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        std::int32_t pixelX = -1;
        std::int32_t pixelY = -1;
    };

    [[nodiscard]] bool MakePlacement(
        const PRISMA_UI_VR_API::SpatialUpdateV1& update,
        WorldPanelPlacement& outPlacement) noexcept;

    [[nodiscard]] bool ResolveSurface(
        const WorldPanelPlacement& placement,
        const Vec3& billboardCameraMidpoint,
        WorldPanelSurface& outSurface) noexcept;

    [[nodiscard]] bool IntersectRay(
        const WorldPanelSurface& surface,
        const Vec3& rayOrigin,
        const Vec3& rayDirection,
        float maxDistance,
        WorldPanelRayHit& outHit) noexcept;
}
