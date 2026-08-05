#include "PrismaUI/WorldPanelGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace PrismaUI::WorldPanelGeometry
{
    namespace
    {
        constexpr float minimumLength = 1.0e-4f;
        constexpr float minimumBillboardDistance = 1.0f;
        constexpr float planeParallelEpsilon = 1.0e-6f;
        constexpr float boundsEpsilonScale =
            8.0f * std::numeric_limits<float>::epsilon();

        [[nodiscard]] bool IsFinite(const Vec3& value) noexcept
        {
            return std::isfinite(value.x) &&
                   std::isfinite(value.y) &&
                   std::isfinite(value.z);
        }

        [[nodiscard]] Vec3 Add(const Vec3& left, const Vec3& right) noexcept
        {
            return {
                left.x + right.x,
                left.y + right.y,
                left.z + right.z
            };
        }

        [[nodiscard]] Vec3 Subtract(const Vec3& left, const Vec3& right) noexcept
        {
            return {
                left.x - right.x,
                left.y - right.y,
                left.z - right.z
            };
        }

        [[nodiscard]] Vec3 Scale(const Vec3& value, float scale) noexcept
        {
            return {value.x * scale, value.y * scale, value.z * scale};
        }

        [[nodiscard]] float Dot(const Vec3& left, const Vec3& right) noexcept
        {
            return left.x * right.x + left.y * right.y + left.z * right.z;
        }

        [[nodiscard]] Vec3 Cross(const Vec3& left, const Vec3& right) noexcept
        {
            return {
                left.y * right.z - left.z * right.y,
                left.z * right.x - left.x * right.z,
                left.x * right.y - left.y * right.x
            };
        }

        [[nodiscard]] bool Normalize(Vec3& value) noexcept
        {
            const auto lengthSquared = Dot(value, value);
            if (!std::isfinite(lengthSquared) ||
                lengthSquared < minimumLength * minimumLength) {
                return false;
            }
            value = Scale(value, 1.0f / std::sqrt(lengthSquared));
            return IsFinite(value);
        }

        [[nodiscard]] bool NormalizeQuaternion(
            std::array<float, 4>& value) noexcept
        {
            float normSquared = 0.0f;
            for (const auto component : value) {
                if (!std::isfinite(component)) {
                    return false;
                }
                normSquared += component * component;
            }
            if (!std::isfinite(normSquared) ||
                normSquared < minimumLength * minimumLength) {
                return false;
            }
            const auto inverseNorm = 1.0f / std::sqrt(normSquared);
            for (auto& component : value) {
                component *= inverseNorm;
            }
            return true;
        }

        [[nodiscard]] Vec3 Rotate(
            const Vec3& vector,
            const std::array<float, 4>& quaternion) noexcept
        {
            const Vec3 imaginary{
                quaternion[0],
                quaternion[1],
                quaternion[2]
            };
            const auto twiceCross = Scale(Cross(imaginary, vector), 2.0f);
            return Add(
                vector,
                Add(
                    Scale(twiceCross, quaternion[3]),
                    Cross(imaginary, twiceCross)));
        }

        [[nodiscard]] bool IsUsable(const WorldPanelSurface& surface) noexcept
        {
            return IsFinite(surface.center) &&
                   IsFinite(surface.right) &&
                   IsFinite(surface.up) &&
                   IsFinite(surface.normal) &&
                   std::isfinite(surface.physicalWidth) &&
                   std::isfinite(surface.physicalHeight) &&
                   surface.physicalWidth > 0.0f &&
                   surface.physicalHeight > 0.0f &&
                   surface.pixelWidth > 0 &&
                   surface.pixelHeight > 0;
        }
    }

    bool MakePlacement(
        const PRISMA_UI_VR_API::SpatialUpdateV1& update,
        WorldPanelPlacement& outPlacement) noexcept
    {
        using enum PRISMA_UI_VR_API::SpatialPresentationMode;

        outPlacement = {};
        const auto reservedIsZero = std::all_of(
            std::begin(update.reserved),
            std::end(update.reserved),
            [](std::uint32_t value) { return value == 0; });
        if (update.structSize != sizeof(update) ||
            update.coordinateSpace !=
                PRISMA_UI_VR_API::SpatialCoordinateSpace::GameWorld ||
            (update.presentationMode != WorldBillboard &&
             update.presentationMode != WorldQuad) ||
            (update.flags &
             ~PRISMA_UI_VR_API::SpatialUpdate_SceneDepthOcclusion) != 0 ||
            update.dimensions.pixelWidth == 0 ||
            update.dimensions.pixelHeight == 0 ||
            !reservedIsZero ||
            !std::isfinite(update.dimensions.physicalWidth) ||
            !std::isfinite(update.dimensions.physicalHeight) ||
            update.dimensions.physicalWidth <= 0.0f ||
            update.dimensions.physicalHeight <= 0.0f) {
            return false;
        }

        WorldPanelPlacement placement{};
        placement.mode = update.presentationMode;
        placement.center = {
            update.pose.position[0],
            update.pose.position[1],
            update.pose.position[2]
        };
        if (!IsFinite(placement.center)) {
            return false;
        }

        std::copy(
            std::begin(update.pose.orientation),
            std::end(update.pose.orientation),
            placement.orientation.begin());
        if (placement.mode == WorldQuad &&
            !NormalizeQuaternion(placement.orientation)) {
            return false;
        }
        if (placement.mode == WorldBillboard) {
            placement.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        }

        placement.pixelWidth = update.dimensions.pixelWidth;
        placement.pixelHeight = update.dimensions.pixelHeight;
        placement.physicalWidth = update.dimensions.physicalWidth;
        placement.physicalHeight = update.dimensions.physicalHeight;
        outPlacement = placement;
        return true;
    }

    bool ResolveSurface(
        const WorldPanelPlacement& placement,
        const Vec3& billboardCameraMidpoint,
        WorldPanelSurface& outSurface) noexcept
    {
        using enum PRISMA_UI_VR_API::SpatialPresentationMode;

        outSurface = {};
        if (!IsFinite(placement.center) ||
            !std::isfinite(placement.physicalWidth) ||
            !std::isfinite(placement.physicalHeight) ||
            placement.physicalWidth <= 0.0f ||
            placement.physicalHeight <= 0.0f ||
            placement.pixelWidth == 0 ||
            placement.pixelHeight == 0) {
            return false;
        }

        Vec3 right{};
        Vec3 up{};
        Vec3 normal{};

        if (placement.mode == WorldQuad) {
            auto orientation = placement.orientation;
            if (!NormalizeQuaternion(orientation)) {
                return false;
            }
            right = Rotate({1.0f, 0.0f, 0.0f}, orientation);
            up = Rotate({0.0f, 1.0f, 0.0f}, orientation);
            normal = Cross(right, up);
            if (!Normalize(right) || !Normalize(up) || !Normalize(normal)) {
                return false;
            }
        } else if (placement.mode == WorldBillboard) {
            if (!IsFinite(billboardCameraMidpoint)) {
                return false;
            }
            normal = Subtract(billboardCameraMidpoint, placement.center);
            const auto distanceSquared = Dot(normal, normal);
            if (!std::isfinite(distanceSquared) ||
                distanceSquared <
                    minimumBillboardDistance * minimumBillboardDistance ||
                !Normalize(normal)) {
                return false;
            }

            Vec3 worldUp{0.0f, 0.0f, 1.0f};
            if (std::fabs(Dot(normal, worldUp)) > 0.95f) {
                worldUp = {0.0f, 1.0f, 0.0f};
            }
            right = Cross(worldUp, normal);
            if (!Normalize(right)) {
                return false;
            }
            up = Cross(normal, right);
            if (!Normalize(up)) {
                return false;
            }
        } else {
            return false;
        }

        WorldPanelSurface surface{};
        surface.center = placement.center;
        surface.right = right;
        surface.up = up;
        surface.normal = normal;
        surface.physicalWidth = placement.physicalWidth;
        surface.physicalHeight = placement.physicalHeight;
        surface.pixelWidth = placement.pixelWidth;
        surface.pixelHeight = placement.pixelHeight;

        const auto halfRight = Scale(right, placement.physicalWidth * 0.5f);
        const auto halfUp = Scale(up, placement.physicalHeight * 0.5f);
        surface.corners = {
            Add(Subtract(placement.center, halfRight), halfUp),
            Add(Add(placement.center, halfRight), halfUp),
            Subtract(Add(placement.center, halfRight), halfUp),
            Subtract(Subtract(placement.center, halfRight), halfUp)
        };

        outSurface = surface;
        return true;
    }

    bool IntersectRay(
        const WorldPanelSurface& surface,
        const Vec3& rayOrigin,
        const Vec3& rayDirection,
        float maxDistance,
        WorldPanelRayHit& outHit) noexcept
    {
        outHit = {};
        outHit.pixelX = -1;
        outHit.pixelY = -1;

        if (!IsUsable(surface) ||
            !IsFinite(rayOrigin) ||
            !IsFinite(rayDirection) ||
            !std::isfinite(maxDistance) ||
            maxDistance <= 0.0f) {
            return false;
        }

        auto direction = rayDirection;
        if (!Normalize(direction)) {
            return false;
        }

        const auto denominator = Dot(direction, surface.normal);
        if (!std::isfinite(denominator) ||
            std::fabs(denominator) <= planeParallelEpsilon) {
            return false;
        }

        const auto distance =
            Dot(Subtract(surface.center, rayOrigin), surface.normal) /
            denominator;
        if (!std::isfinite(distance) ||
            distance < 0.0f ||
            distance > maxDistance) {
            return false;
        }

        const auto position = Add(rayOrigin, Scale(direction, distance));
        const auto relative = Subtract(position, surface.center);
        const auto horizontal = Dot(relative, surface.right);
        const auto vertical = Dot(relative, surface.up);
        const auto halfWidth = surface.physicalWidth * 0.5f;
        const auto halfHeight = surface.physicalHeight * 0.5f;
        const auto boundsEpsilon =
            (std::max)(surface.physicalWidth, surface.physicalHeight) *
            boundsEpsilonScale;
        if (!std::isfinite(horizontal) ||
            !std::isfinite(vertical) ||
            horizontal < -halfWidth - boundsEpsilon ||
            horizontal > halfWidth + boundsEpsilon ||
            vertical < -halfHeight - boundsEpsilon ||
            vertical > halfHeight + boundsEpsilon) {
            return false;
        }

        const auto u = std::clamp(
            horizontal / surface.physicalWidth + 0.5f,
            0.0f,
            1.0f);
        const auto v = std::clamp(
            0.5f - vertical / surface.physicalHeight,
            0.0f,
            1.0f);
        const auto pixelX = (std::min)(
            static_cast<std::uint32_t>(
                u * static_cast<float>(surface.pixelWidth)),
            surface.pixelWidth - 1);
        const auto pixelY = (std::min)(
            static_cast<std::uint32_t>(
                v * static_cast<float>(surface.pixelHeight)),
            surface.pixelHeight - 1);

        outHit.position = position;
        outHit.distance = distance;
        outHit.u = u;
        outHit.v = v;
        outHit.pixelX = static_cast<std::int32_t>(pixelX);
        outHit.pixelY = static_cast<std::int32_t>(pixelY);
        return true;
    }
}
