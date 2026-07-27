#pragma once

#include <cstdint>
#include <limits>

namespace PrismaUI::PresentationThreadPolicy
{
    struct Observation
    {
        bool first = false;
        bool migrated = false;
        std::uint64_t migrationCount = 0;
    };

    // FO4VR owns the stereo-submission caller and may move that serialized
    // boundary between engine threads. Thread identity is diagnostic state,
    // not an admission requirement; presentationMutex provides exclusivity.
    template <class ThreadToken>
    class SerializedTracker
    {
    public:
        [[nodiscard]] Observation Observe(
            const ThreadToken& current) noexcept
        {
            Observation result;
            if (current == ThreadToken{}) {
                return result;
            }
            if (last_ == ThreadToken{}) {
                last_ = current;
                result.first = true;
                return result;
            }
            if (last_ != current) {
                last_ = current;
                if (migrationCount_ !=
                    (std::numeric_limits<std::uint64_t>::max)()) {
                    ++migrationCount_;
                }
                result.migrated = true;
            }
            result.migrationCount = migrationCount_;
            return result;
        }

    private:
        ThreadToken last_{};
        std::uint64_t migrationCount_ = 0;
    };
}
