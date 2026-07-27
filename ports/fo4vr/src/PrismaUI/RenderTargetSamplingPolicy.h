#pragma once

namespace PrismaUI::RenderTargetSamplingPolicy
{
    struct Plan
    {
        bool valid = false;
        bool resolveBeforeSampling = false;
        bool suspendBoundTarget = false;
    };

    [[nodiscard]] constexpr Plan MakePlan(
        bool samplesBoundLogicalTarget,
        bool resolveDirty,
        bool physicalResourcesAlias) noexcept
    {
        if (physicalResourcesAlias) {
            return {};
        }
        return {
            .valid = true,
            .resolveBeforeSampling = resolveDirty,
            .suspendBoundTarget =
                samplesBoundLogicalTarget && resolveDirty
        };
    }
}
