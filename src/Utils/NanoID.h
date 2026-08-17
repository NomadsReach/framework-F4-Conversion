#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

class NanoIdGenerator {
public:
    std::uint64_t generate()
    {
        std::uint64_t current = next_.load(std::memory_order_relaxed);
        while (true) {
            if (current == 0 || current == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("PrismaUI view ID space exhausted");
            }
            if (next_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                return current;
            }
        }
    }

private:
    std::atomic<std::uint64_t> next_{1};
};
