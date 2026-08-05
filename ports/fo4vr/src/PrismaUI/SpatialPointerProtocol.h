#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace PrismaUI::SpatialPointerProtocol
{
    enum class EventKind : std::uint8_t
    {
        Move,
        Down,
        Up,
        Scroll
    };

    struct Event
    {
        EventKind kind = EventKind::Move;
        int x = -1;
        int y = -1;
        int deltaX = 0;
        int deltaY = 0;
        bool forced = false;
        bool buttonDown = false;
    };

    struct EventBuffer
    {
        static constexpr std::size_t kCapacity = 6;

        std::array<Event, kCapacity> events{};
        std::size_t count = 0;

        [[nodiscard]] bool Push(const Event& event) noexcept
        {
            if (count == events.size()) {
                return false;
            }
            events[count++] = event;
            return true;
        }
    };

    struct State
    {
        bool hover = false;
        bool captured = false;
        bool previousPrimaryDown = false;
        bool requiresPrimaryRelease = true;
        int lastPixelX = -1;
        int lastPixelY = -1;
        bool filterValid = false;
        float filteredPixelX = 0.0f;
        float filteredPixelY = 0.0f;
        std::uint64_t filteredSequence = 0;
        bool dragStarted = false;
        int pressPixelX = -1;
        int pressPixelY = -1;
    };

    struct Sample
    {
        bool active = false;
        bool backendReady = false;
        bool primaryDown = false;
        bool inside = false;
        int hitPixelX = -1;
        int hitPixelY = -1;
        bool hasCapturedPlane = false;
        int capturedPixelX = -1;
        int capturedPixelY = -1;
        int scrollDeltaX = 0;
        int scrollDeltaY = 0;
        std::uint64_t sequence = 0;
        std::uint32_t pixelWidth = 0;
        std::uint32_t pixelHeight = 0;
    };

    enum class ApplyDisposition : std::uint8_t
    {
        Applied,
        NotReady
    };

    struct ApplyResult
    {
        bool success = false;
        ApplyDisposition disposition = ApplyDisposition::NotReady;
    };

    struct QueueSampleKey
    {
        std::uint32_t flags = 0;
        std::uint32_t buttonLevels = 0;
        int scrollDeltaX = 0;
        int scrollDeltaY = 0;
        std::uint32_t pointerSourceId = 0;
    };

    [[nodiscard]] inline bool MayCoalesce(
        const QueueSampleKey& previous,
        const QueueSampleKey& next,
        std::uint32_t activeFlag) noexcept
    {
        return (previous.flags & activeFlag) == (next.flags & activeFlag) &&
               previous.buttonLevels == next.buttonLevels &&
               previous.pointerSourceId == next.pointerSourceId &&
               previous.scrollDeltaX == 0 &&
               previous.scrollDeltaY == 0 &&
               next.scrollDeltaX == 0 &&
               next.scrollDeltaY == 0;
    }

    inline void ResetMotion(State& state) noexcept
    {
        state.filterValid = false;
        state.filteredPixelX = 0.0f;
        state.filteredPixelY = 0.0f;
        state.filteredSequence = 0;
        state.dragStarted = false;
        state.pressPixelX = -1;
        state.pressPixelY = -1;
    }

    struct PixelPoint
    {
        int x = -1;
        int y = -1;
        bool valid = false;
    };

    [[nodiscard]] inline PixelPoint Stabilize(
        State& state,
        const Sample& sample,
        int rawX,
        int rawY) noexcept
    {
        const auto valid =
            sample.pixelWidth > 0 &&
            sample.pixelHeight > 0 &&
            rawX >= 0 &&
            rawY >= 0 &&
            static_cast<std::uint32_t>(rawX) < sample.pixelWidth &&
            static_cast<std::uint32_t>(rawY) < sample.pixelHeight;
        if (!valid) {
            return {rawX, rawY, false};
        }

        if (!state.filterValid) {
            state.filterValid = true;
            state.filteredPixelX = static_cast<float>(rawX);
            state.filteredPixelY = static_cast<float>(rawY);
            state.filteredSequence = sample.sequence;
        } else if (
            sample.sequence == 0 ||
            sample.sequence != state.filteredSequence) {
            const auto deltaX =
                static_cast<float>(rawX) - state.filteredPixelX;
            const auto deltaY =
                static_cast<float>(rawY) - state.filteredPixelY;
            const auto distance = std::sqrt(
                deltaX * deltaX + deltaY * deltaY);
            const auto referenceExtent = static_cast<float>(
                (std::min)(sample.pixelWidth, sample.pixelHeight));
            const auto fullResponseDistance = std::clamp(
                referenceExtent * 0.08f,
                24.0f,
                96.0f);
            const auto normalizedSpeed = std::clamp(
                distance / fullResponseDistance,
                0.0f,
                1.0f);
            const auto alpha = 0.16f + 0.84f * normalizedSpeed;
            state.filteredPixelX += deltaX * alpha;
            state.filteredPixelY += deltaY * alpha;
            state.filteredSequence = sample.sequence;
        }

        return {
            static_cast<int>(std::lround(state.filteredPixelX)),
            static_cast<int>(std::lround(state.filteredPixelY)),
            true
        };
    }

    [[nodiscard]] inline int ClickSlopPixels(const Sample& sample) noexcept
    {
        if (sample.pixelWidth == 0 || sample.pixelHeight == 0) {
            return 12;
        }
        const auto referenceExtent = static_cast<float>(
            (std::min)(sample.pixelWidth, sample.pixelHeight));
        return static_cast<int>(std::lround(std::clamp(
            referenceExtent * 0.05f,
            12.0f,
            48.0f)));
    }

    [[nodiscard]] inline bool Cancel(
        State& state,
        EventBuffer& output,
        bool forced,
        bool requireNeutral) noexcept
    {
        if (state.captured) {
            if (!output.Push({
                    EventKind::Move,
                    -1,
                    -1,
                    0,
                    0,
                    forced,
                    true
                }) ||
                !output.Push({
                    EventKind::Up,
                    -1,
                    -1,
                    0,
                    0,
                    forced,
                    false
                })) {
                return false;
            }
        } else if (
            state.hover &&
            !output.Push({
                EventKind::Move,
                -1,
                -1,
                0,
                0,
                forced,
                false
            })) {
            return false;
        }

        state.hover = false;
        state.captured = false;
        state.previousPrimaryDown = false;
        state.requiresPrimaryRelease = requireNeutral;
        state.lastPixelX = -1;
        state.lastPixelY = -1;
        ResetMotion(state);
        return true;
    }

    [[nodiscard]] inline bool DenyCentralRoute(
        State& state,
        bool primaryDown,
        EventBuffer& output) noexcept
    {
        if (!Cancel(state, output, state.captured, true)) {
            return false;
        }
        state.previousPrimaryDown = primaryDown;
        return true;
    }

    [[nodiscard]] inline ApplyResult Apply(
        State& state,
        const Sample& sample,
        EventBuffer& output) noexcept
    {
        if (!sample.active) {
            if (!Cancel(state, output, true, false)) {
                return {};
            }
            return {true, ApplyDisposition::Applied};
        }

        if (!sample.backendReady) {
            if (!Cancel(state, output, true, true)) {
                return {};
            }
            state.previousPrimaryDown = sample.primaryDown;
            return {true, ApplyDisposition::NotReady};
        }

        if (!sample.primaryDown) {
            state.requiresPrimaryRelease = false;
        }
        const auto risingPrimary =
            sample.primaryDown && !state.previousPrimaryDown;
        const auto fallingPrimary =
            !sample.primaryDown && state.previousPrimaryDown;

        int moveX = -1;
        int moveY = -1;
        bool hasMovePosition = false;
        if (sample.inside) {
            moveX = sample.hitPixelX;
            moveY = sample.hitPixelY;
            hasMovePosition = true;
        } else if (state.captured && sample.hasCapturedPlane) {
            moveX = sample.capturedPixelX;
            moveY = sample.capturedPixelY;
            hasMovePosition = true;
        }

        if (hasMovePosition) {
            const auto stabilized =
                Stabilize(state, sample, moveX, moveY);
            moveX = stabilized.x;
            moveY = stabilized.y;

            if (state.captured && !state.dragStarted) {
                if (!stabilized.valid) {
                    state.dragStarted = true;
                } else {
                    const auto deltaX =
                        static_cast<std::int64_t>(moveX) -
                        state.pressPixelX;
                    const auto deltaY =
                        static_cast<std::int64_t>(moveY) -
                        state.pressPixelY;
                    const auto slop =
                        static_cast<std::int64_t>(ClickSlopPixels(sample));
                    state.dragStarted =
                        deltaX * deltaX + deltaY * deltaY > slop * slop;
                }
                if (!state.dragStarted) {
                    moveX = state.pressPixelX;
                    moveY = state.pressPixelY;
                }
            }
        }

        if (sample.inside) {
            if (!output.Push({
                    EventKind::Move,
                    moveX,
                    moveY,
                    0,
                    0,
                    false,
                    state.captured
                })) {
                return {};
            }
            state.hover = true;
            state.lastPixelX = moveX;
            state.lastPixelY = moveY;
        } else {
            if (state.hover &&
                !state.captured &&
                !output.Push({EventKind::Move, -1, -1})) {
                return {};
            }
            state.hover = false;
            if (state.captured) {
                if (!output.Push({
                        EventKind::Move,
                        moveX,
                        moveY,
                        0,
                        0,
                        false,
                        true
                    })) {
                    return {};
                }
                state.lastPixelX = moveX;
                state.lastPixelY = moveY;
            } else {
                ResetMotion(state);
            }
        }

        if (risingPrimary &&
            sample.inside &&
            !state.requiresPrimaryRelease) {
            if (!output.Push({
                    EventKind::Down,
                    state.lastPixelX,
                    state.lastPixelY
                })) {
                return {};
            }
            state.captured = true;
            state.dragStarted = false;
            state.pressPixelX = state.lastPixelX;
            state.pressPixelY = state.lastPixelY;
        }

        if (fallingPrimary && state.captured) {
            if (!output.Push({
                    EventKind::Up,
                    state.lastPixelX,
                    state.lastPixelY
                })) {
                return {};
            }
            state.captured = false;
            state.dragStarted = false;
            state.pressPixelX = -1;
            state.pressPixelY = -1;
            if (!sample.inside) {
                ResetMotion(state);
            }
        }

        if (sample.inside &&
            (sample.scrollDeltaX != 0 || sample.scrollDeltaY != 0) &&
            !output.Push({
                EventKind::Scroll,
                state.lastPixelX,
                state.lastPixelY,
                sample.scrollDeltaX,
                sample.scrollDeltaY
            })) {
            return {};
        }

        state.previousPrimaryDown = sample.primaryDown;
        return {true, ApplyDisposition::Applied};
    }
}
