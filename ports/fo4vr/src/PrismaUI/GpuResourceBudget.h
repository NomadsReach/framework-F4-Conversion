#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace PrismaUI::GpuResourceBudget
{
    // Framework admission normally remains below 24M final pixels. The
    // compositor can retain old and replacement targets during a resize, so
    // keep enough bounded overlap for that valid transition. Each logical
    // pixel owns one resolved BGRA8 pixel and the mandatory 8x MSAA source.
    inline constexpr std::size_t kMaximumRenderTargetPixels =
        64ull * 1024ull * 1024ull;

    struct RenderTargetAllocation
    {
        bool valid = false;
        std::size_t pixels = 0;
    };

    [[nodiscard]] constexpr RenderTargetAllocation
    PlanRenderTargetAllocation(
        std::size_t livePixels,
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        if (width == 0 || height == 0) {
            return {};
        }

        constexpr auto maximum =
            (std::numeric_limits<std::size_t>::max)();
        if (height > maximum / width) {
            return {};
        }
        const auto pixels =
            static_cast<std::size_t>(width) * height;
        if (pixels > kMaximumRenderTargetPixels ||
            livePixels >
                kMaximumRenderTargetPixels - pixels) {
            return {};
        }

        return {
            .valid = true,
            .pixels = pixels
        };
    }
}
