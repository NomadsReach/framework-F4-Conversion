#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace PrismaUI::SpatialPointerRouter
{
    struct Candidate
    {
        std::uint64_t viewId = 0;
        std::uint32_t sourceId = 0;
        int drawOrder = 0;
        bool active = false;
        bool operational = false;
        bool captured = false;
        bool hit = false;
        float hitDistance = 0.0f;
    };

    [[nodiscard]] inline bool SelectWinners(
        std::span<const Candidate> candidates,
        std::span<std::uint8_t> winners) noexcept
    {
        if (winners.size() < candidates.size()) {
            return false;
        }
        std::fill_n(winners.begin(), candidates.size(), std::uint8_t{0});

        for (std::size_t first = 0; first < candidates.size(); ++first) {
            const auto source = candidates[first].sourceId;
            if (source == 0) {
                winners[first] = 1;
                continue;
            }

            bool handled = false;
            for (std::size_t previous = 0; previous < first; ++previous) {
                if (candidates[previous].sourceId == source) {
                    handled = true;
                    break;
                }
            }
            if (handled) {
                continue;
            }

            std::size_t winner = candidates.size();
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const auto& candidate = candidates[index];
                if (candidate.sourceId != source ||
                    !candidate.active ||
                    !candidate.operational ||
                    !candidate.captured) {
                    continue;
                }
                if (winner == candidates.size() ||
                    candidate.drawOrder > candidates[winner].drawOrder ||
                    (candidate.drawOrder == candidates[winner].drawOrder &&
                     candidate.viewId > candidates[winner].viewId)) {
                    winner = index;
                }
            }

            if (winner == candidates.size()) {
                auto nearest = std::numeric_limits<float>::infinity();
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    const auto& candidate = candidates[index];
                    if (candidate.sourceId != source ||
                        !candidate.active ||
                        !candidate.operational ||
                        !candidate.hit ||
                        !std::isfinite(candidate.hitDistance) ||
                        candidate.hitDistance < 0.0f) {
                        continue;
                    }

                    if (candidate.hitDistance < nearest ||
                        (candidate.hitDistance == nearest &&
                         (winner == candidates.size() ||
                          candidate.drawOrder > candidates[winner].drawOrder ||
                          (candidate.drawOrder ==
                               candidates[winner].drawOrder &&
                           candidate.viewId >
                               candidates[winner].viewId)))) {
                        nearest = candidate.hitDistance;
                        winner = index;
                    }
                }
            }

            if (winner != candidates.size()) {
                winners[winner] = 1;
            }
        }
        return true;
    }

    [[nodiscard]] inline std::size_t FindWinnerForSource(
        std::span<const Candidate> candidates,
        std::span<const std::uint8_t> winners,
        std::uint32_t sourceId) noexcept
    {
        if (winners.size() < candidates.size()) {
            return candidates.size();
        }
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (winners[index] != 0 &&
                candidates[index].sourceId == sourceId &&
                candidates[index].active &&
                candidates[index].operational) {
                return index;
            }
        }
        return candidates.size();
    }
}
