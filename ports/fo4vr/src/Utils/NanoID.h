#pragma once

#include <atomic>
#include <cstdint>

using NanoId = std::uint64_t;

class NanoIdGenerator final
{
public:
    [[nodiscard]] NanoId generate() noexcept
    {
        auto value = next_.fetch_add(1, std::memory_order_relaxed);
        if (value == 0) {
            value = next_.fetch_add(1, std::memory_order_relaxed);
        }
        return value;
    }

    NanoIdGenerator() = default;
    NanoIdGenerator(const NanoIdGenerator&) = delete;
    NanoIdGenerator& operator=(const NanoIdGenerator&) = delete;

private:
    std::atomic<NanoId> next_{1};
};
