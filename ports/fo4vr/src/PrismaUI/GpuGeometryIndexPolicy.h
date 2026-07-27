#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace PrismaUI::GpuGeometryIndexPolicy
{
    inline constexpr std::size_t kIndexBytes =
        sizeof(std::uint32_t);

    [[nodiscard]] inline bool BuildInvalidIndexPrefix(
        std::vector<std::uint32_t>& prefix,
        const std::byte* indexData,
        std::size_t indexBytes,
        std::size_t vertexCount) noexcept
    {
        if (!indexData ||
            indexBytes == 0 ||
            indexBytes % kIndexBytes != 0) {
            return false;
        }

        const auto indexCount = indexBytes / kIndexBytes;
        if (indexCount >=
            (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }

        try {
            prefix.resize(indexCount + 1);
        } catch (...) {
            return false;
        }

        prefix[0] = 0;
        for (std::size_t index = 0;
             index < indexCount;
             ++index) {
            std::uint32_t value = 0;
            std::memcpy(
                &value,
                indexData + index * kIndexBytes,
                kIndexBytes);
            prefix[index + 1] =
                prefix[index] +
                (value >= vertexCount ? 1u : 0u);
        }
        return true;
    }

    [[nodiscard]] inline bool DrawRangeIsValid(
        const std::vector<std::uint32_t>& prefix,
        std::size_t indexBytes,
        std::uint32_t offset,
        std::uint32_t count) noexcept
    {
        if (prefix.empty()) {
            return false;
        }

        const auto indexCount = prefix.size() - 1;
        if (indexCount >
                (std::numeric_limits<std::size_t>::max)() /
                    kIndexBytes ||
            indexBytes != indexCount * kIndexBytes) {
            return false;
        }

        const auto first = static_cast<std::size_t>(offset);
        const auto consumed = static_cast<std::size_t>(count);
        return first <= indexCount &&
               consumed <= indexCount - first;
    }

    [[nodiscard]] inline bool
    DrawRangeReferencesExistingVertices(
        const std::vector<std::uint32_t>& prefix,
        std::size_t indexBytes,
        std::uint32_t offset,
        std::uint32_t count) noexcept
    {
        if (!DrawRangeIsValid(
                prefix,
                indexBytes,
                offset,
                count)) {
            return false;
        }

        const auto first = static_cast<std::size_t>(offset);
        const auto last =
            first + static_cast<std::size_t>(count);
        return prefix[first] == prefix[last];
    }
}
