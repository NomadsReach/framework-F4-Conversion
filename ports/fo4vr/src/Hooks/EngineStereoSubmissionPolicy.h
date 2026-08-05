#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Hooks::EngineStereoSubmissionPolicy
{
    inline constexpr std::uintptr_t kSubmitStereoTextureRva = 0x1BABE90;
    inline constexpr std::uintptr_t kFullSubmitCallsiteRva = 0x1D8DB22;
    inline constexpr std::uintptr_t kSpecialSubmitCallsiteRva = 0x1D8DC25;
    inline constexpr std::size_t kCallsitePrefixSize = 10;

    inline constexpr std::array<std::uint8_t, 5> kFullSubmitOriginalCall{
        0xE8, 0x69, 0xE3, 0xE1, 0xFF
    };
    inline constexpr std::array<std::uint8_t, 5> kSpecialSubmitOriginalCall{
        0xE8, 0x66, 0xE2, 0xE1, 0xFF
    };

    inline constexpr std::array<std::uint8_t, 22> kFullSubmitBoundary{
        0x03, 0xC9, 0x48, 0x8B, 0x8C, 0xCB, 0x68, 0x0A, 0x00, 0x00,
        0xE8, 0x69, 0xE3, 0xE1, 0xFF,
        0x80, 0x3D, 0x1A, 0xE0, 0xAF, 0x01, 0x00
    };
    inline constexpr std::array<std::uint8_t, 22> kSpecialSubmitBoundary{
        0x03, 0xC9, 0x48, 0x8B, 0x8C, 0xCB, 0x68, 0x0A, 0x00, 0x00,
        0xE8, 0x66, 0xE2, 0xE1, 0xFF,
        0x80, 0x3D, 0x17, 0xDF, 0xAF, 0x01, 0x00
    };
    inline constexpr std::array<std::uint8_t, 16> kSubmitStereoTexturePrologue{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81,
        0xEC, 0x60, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xF9
    };

    struct Bounds
    {
        float uMin;
        float vMin;
        float uMax;
        float vMax;
    };

    inline constexpr Bounds kLeftBounds{0.0f, 0.0f, 0.5f, 1.0f};
    inline constexpr Bounds kRightBounds{0.5f, 0.0f, 1.0f, 1.0f};

    [[nodiscard]] constexpr std::uintptr_t RelativeCallTarget(
        std::uintptr_t callsite,
        std::int32_t displacement) noexcept
    {
        return static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(callsite + 5) + displacement);
    }
}
