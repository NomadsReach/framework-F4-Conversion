#include "PCH.h"

#include "PrismaUI/SpatialPointer.h"

#include "PrismaUI/InputHandler.h"
#include "PrismaUI/SpatialPointerRouter.h"
#include "PrismaUI/SpatialPresentation.h"

namespace PrismaUI::SpatialPointer
{
    namespace
    {
        using PRISMA_UI_VR_API::SpatialPointerUpdateV1;
        using PRISMA_UI_VR_API::SpatialResult;
        using WorldPanelGeometry::Vec3;
        using WorldPanelGeometry::WorldPanelRayHit;
        using WorldPanelGeometry::WorldPanelSurface;

        constexpr auto kInactiveTimeout =
            std::chrono::milliseconds(500);
        constexpr float kMaximumRayMagnitude = 1.0e8f;
        constexpr float kMinimumDirectionNormSquared = 1.0e-8f;

        struct Prepared
        {
            FrameTarget* target = nullptr;
            std::array<
                SpatialPointerUpdateV1,
                Core::SpatialPointerRuntimeState::kSampleCapacity>
                samples{};
            std::size_t sampleCount = 0;
            SpatialPointerUpdateV1 routingSample{};
            bool hasRoutingSample = false;
            bool operational = false;
            bool timedOut = false;
            bool captured = false;
            WorldPanelRayHit routingHit{};
            bool routingHasHit = false;
        };

        [[nodiscard]] bool IsFiniteBounded(
            float value) noexcept
        {
            return std::isfinite(value) &&
                   std::fabs(value) <= kMaximumRayMagnitude;
        }

        [[nodiscard]] bool ValidateAndNormalize(
            SpatialPointerUpdateV1& update) noexcept
        {
            using namespace PRISMA_UI_VR_API;

            if (update.coordinateSpace !=
                    SpatialCoordinateSpace::GameWorld ||
                (update.flags & ~SpatialPointerUpdate_Active) != 0 ||
                (update.buttonLevels &
                 ~SpatialPointerButton_Primary) != 0 ||
                update.sequence == 0 ||
                std::any_of(
                    std::begin(update.reserved),
                    std::end(update.reserved),
                    [](std::uint32_t value) {
                        return value != 0;
                    })) {
                return false;
            }

            if ((update.flags &
                 SpatialPointerUpdate_Active) == 0) {
                update.buttonLevels = 0;
                update.scrollDeltaX = 0;
                update.scrollDeltaY = 0;
                update.rayOrigin[0] = 0.0f;
                update.rayOrigin[1] = 0.0f;
                update.rayOrigin[2] = 0.0f;
                update.rayDirection[0] = 0.0f;
                update.rayDirection[1] = 0.0f;
                update.rayDirection[2] = 1.0f;
                update.maxDistance = 0.0f;
                return true;
            }

            if (!IsFiniteBounded(update.maxDistance) ||
                update.maxDistance <= 0.0f) {
                return false;
            }

            float directionNormSquared = 0.0f;
            for (std::size_t index = 0; index < 3; ++index) {
                if (!IsFiniteBounded(update.rayOrigin[index]) ||
                    !IsFiniteBounded(
                        update.rayDirection[index])) {
                    return false;
                }
                directionNormSquared +=
                    update.rayDirection[index] *
                    update.rayDirection[index];
            }
            if (!std::isfinite(directionNormSquared) ||
                directionNormSquared <
                    kMinimumDirectionNormSquared) {
                return false;
            }
            const auto inverseNorm =
                1.0f / std::sqrt(directionNormSquared);
            for (auto& component : update.rayDirection) {
                component *= inverseNorm;
            }
            return true;
        }

        [[nodiscard]] SpatialPointerProtocol::QueueSampleKey
            QueueKey(
                const SpatialPointerUpdateV1& update) noexcept
        {
            return {
                .flags = update.flags,
                .buttonLevels = update.buttonLevels,
                .scrollDeltaX = update.scrollDeltaX,
                .scrollDeltaY = update.scrollDeltaY,
                .pointerSourceId = update.pointerSourceId
            };
        }

        [[nodiscard]] bool IntersectCapturedPlane(
            const WorldPanelSurface& surface,
            const SpatialPointerUpdateV1& update,
            int& pixelX,
            int& pixelY) noexcept
        {
            const Vec3 origin{
                update.rayOrigin[0],
                update.rayOrigin[1],
                update.rayOrigin[2]
            };
            const Vec3 direction{
                update.rayDirection[0],
                update.rayDirection[1],
                update.rayDirection[2]
            };
            const auto dot = [](const Vec3& left, const Vec3& right) {
                return left.x * right.x +
                       left.y * right.y +
                       left.z * right.z;
            };
            const auto subtract = [](const Vec3& left, const Vec3& right) {
                return Vec3{
                    left.x - right.x,
                    left.y - right.y,
                    left.z - right.z
                };
            };
            const auto denominator =
                dot(direction, surface.normal);
            if (!std::isfinite(denominator) ||
                std::fabs(denominator) <= 1.0e-6f) {
                return false;
            }
            const auto distance =
                dot(
                    subtract(surface.center, origin),
                    surface.normal) /
                denominator;
            if (!std::isfinite(distance) ||
                distance < 0.0f ||
                distance > update.maxDistance) {
                return false;
            }
            const Vec3 position{
                origin.x + direction.x * distance,
                origin.y + direction.y * distance,
                origin.z + direction.z * distance
            };
            const auto relative =
                subtract(position, surface.center);
            const auto u =
                dot(relative, surface.right) /
                    surface.physicalWidth +
                0.5f;
            const auto v =
                0.5f -
                dot(relative, surface.up) /
                    surface.physicalHeight;
            if (!std::isfinite(u) || !std::isfinite(v)) {
                return false;
            }
            pixelX = static_cast<int>(std::lround(
                u * static_cast<float>(surface.pixelWidth)));
            pixelY = static_cast<int>(std::lround(
                v * static_cast<float>(surface.pixelHeight)));
            return true;
        }

        [[nodiscard]] bool Hit(
            const WorldPanelSurface* surface,
            const SpatialPointerUpdateV1& update,
            WorldPanelRayHit& hit) noexcept
        {
            if (!surface ||
                (update.flags &
                 PRISMA_UI_VR_API::
                     SpatialPointerUpdate_Active) == 0) {
                return false;
            }
            return WorldPanelGeometry::IntersectRay(
                *surface,
                {
                    update.rayOrigin[0],
                    update.rayOrigin[1],
                    update.rayOrigin[2]
                },
                {
                    update.rayDirection[0],
                    update.rayDirection[1],
                    update.rayDirection[2]
                },
                update.maxDistance,
                hit);
        }

        [[nodiscard]] bool AppendProtocolEvents(
            Core::PrismaViewId viewId,
            const SpatialPointerProtocol::EventBuffer& events,
            std::span<InputHandler::SpatialPointerEvent> destination,
            std::size_t& destinationCount) noexcept
        {
            if (destinationCount > destination.size() ||
                events.count >
                destination.size() - destinationCount) {
                return false;
            }
            for (std::size_t index = 0;
                 index < events.count;
                 ++index) {
                const auto& source = events.events[index];
                auto& converted =
                    destination[destinationCount + index];
                converted.viewId = viewId;
                converted.x = source.x;
                converted.y = source.y;
                converted.deltaX = source.deltaX;
                converted.deltaY = source.deltaY;
                converted.forced = source.forced;
                converted.buttonDown = source.buttonDown;
                switch (source.kind) {
                case SpatialPointerProtocol::EventKind::Move:
                    converted.kind =
                        InputHandler::
                            SpatialPointerEventKind::Move;
                    break;
                case SpatialPointerProtocol::EventKind::Down:
                    converted.kind =
                        InputHandler::
                            SpatialPointerEventKind::Down;
                    break;
                case SpatialPointerProtocol::EventKind::Up:
                    converted.kind =
                        InputHandler::
                            SpatialPointerEventKind::Up;
                    break;
                case SpatialPointerProtocol::EventKind::Scroll:
                    converted.kind =
                        InputHandler::
                            SpatialPointerEventKind::Scroll;
                    break;
                }
            }
            destinationCount += events.count;
            return true;
        }

        [[nodiscard]] bool QueueProtocolEvents(
            Core::PrismaViewId viewId,
            const SpatialPointerProtocol::EventBuffer& events) noexcept
        {
            std::array<
                InputHandler::SpatialPointerEvent,
                SpatialPointerProtocol::EventBuffer::kCapacity>
                converted{};
            std::size_t count = 0;
            if (!AppendProtocolEvents(
                    viewId,
                    events,
                    converted,
                    count)) {
                return false;
            }
            return InputHandler::EnqueueSpatialPointerEvents(
                std::span(converted).first(count));
        }

        [[nodiscard]] bool ApplyCancel(
            const std::shared_ptr<Core::PrismaView>& view,
            bool forced,
            bool requireNeutral) noexcept
        {
            if (!view) {
                return false;
            }
            std::lock_guard lock(view->spatialPointerMutex);
            auto next = view->spatialPointer.interaction;
            SpatialPointerProtocol::EventBuffer events;
            if (!SpatialPointerProtocol::Cancel(
                    next,
                    events,
                    forced,
                    requireNeutral) ||
                !QueueProtocolEvents(view->id, events)) {
                return false;
            }

            view->spatialPointer.interaction = next;
            view->spatialPointer.sampleHead = 0;
            view->spatialPointer.sampleCount = 0;
            view->spatialPointer.hasActive = false;
            view->spatialPointer.backendReady = false;
            view->spatialPointer.routed = false;
            view->spatialPointer.hasHit = false;
            view->spatialPointer.hitDistance = 0.0f;
            view->spatialPointer.hitUv[0] = 0.0f;
            view->spatialPointer.hitUv[1] = 0.0f;
            view->spatialPointer.lastApplyResult =
                SpatialResult::Ok;
            return true;
        }

        void Prepare(
            FrameTarget& target,
            Prepared& prepared) noexcept
        {
            prepared = {};
            prepared.target = &target;
            target.reticle = {};
            if (!target.view) {
                return;
            }

            auto& state = target.view->spatialPointer;
            std::lock_guard lock(target.view->spatialPointerMutex);
            prepared.captured = state.interaction.captured;
            const auto now = std::chrono::steady_clock::now();
            for (std::size_t index = 0;
                 index < state.sampleCount;
                 ++index) {
                const auto source =
                    (state.sampleHead + index) %
                    state.samples.size();
                prepared.samples[prepared.sampleCount++] =
                    state.samples[source];
            }
            state.sampleHead = 0;
            state.sampleCount = 0;

            if (prepared.sampleCount > 0) {
                state.active =
                    prepared.samples[prepared.sampleCount - 1];
                state.hasActive = true;
            } else if (state.hasActive) {
                prepared.samples[0] = state.active;
                prepared.samples[0].scrollDeltaX = 0;
                prepared.samples[0].scrollDeltaY = 0;
                prepared.sampleCount = 1;
            }

            if (state.hasActive &&
                now - state.lastSubmissionTime >
                    kInactiveTimeout) {
                prepared.samples[0] = state.active;
                prepared.samples[0].flags &=
                    ~PRISMA_UI_VR_API::
                        SpatialPointerUpdate_Active;
                prepared.samples[0].buttonLevels = 0;
                prepared.samples[0].scrollDeltaX = 0;
                prepared.samples[0].scrollDeltaY = 0;
                prepared.sampleCount = 1;
                prepared.timedOut = true;
            }

            if (prepared.sampleCount == 0) {
                return;
            }
            prepared.routingSample =
                prepared.samples[prepared.sampleCount - 1];
            prepared.hasRoutingSample = true;
            prepared.operational =
                target.backendReady &&
                target.surface &&
                !target.view->hidden.load(
                    std::memory_order_acquire) &&
                !target.view->destroying.load(
                    std::memory_order_acquire);
            prepared.routingHasHit =
                prepared.operational &&
                Hit(
                    target.surface,
                    prepared.routingSample,
                    prepared.routingHit);
        }

        void ProcessPrepared(
            Prepared& prepared,
            bool winner) noexcept
        {
            auto* target = prepared.target;
            if (!target || !target->view ||
                !prepared.hasRoutingSample) {
                return;
            }

            auto& state = target->view->spatialPointer;
            std::lock_guard lock(target->view->spatialPointerMutex);
            auto interaction = state.interaction;

            if (!winner &&
                prepared.routingSample.pointerSourceId != 0) {
                SpatialPointerProtocol::EventBuffer events;
                const auto primaryDown =
                    (prepared.routingSample.buttonLevels &
                     PRISMA_UI_VR_API::
                         SpatialPointerButton_Primary) != 0;
                if (SpatialPointerProtocol::DenyCentralRoute(
                        interaction,
                        primaryDown,
                        events) &&
                    QueueProtocolEvents(
                        target->view->id,
                        events)) {
                    state.interaction = interaction;
                    state.active = prepared.routingSample;
                    state.hasActive =
                        !prepared.timedOut &&
                        (prepared.routingSample.flags &
                         PRISMA_UI_VR_API::
                             SpatialPointerUpdate_Active) != 0;
                    state.backendReady =
                        prepared.operational;
                    state.routed = false;
                    state.hasHit = false;
                    state.appliedSequence =
                        prepared.routingSample.sequence;
                    state.lastApplyResult =
                        prepared.operational ?
                            SpatialResult::Ok :
                            SpatialResult::NotReady;
                }
                return;
            }

            WorldPanelRayHit lastHit{};
            bool hasLastHit = false;
            bool allApplied = true;
            std::uint64_t appliedSequence =
                state.appliedSequence;
            auto lastApplyResult =
                state.lastApplyResult;
            std::array<
                InputHandler::SpatialPointerEvent,
                Core::SpatialPointerRuntimeState::kSampleCapacity *
                    SpatialPointerProtocol::EventBuffer::kCapacity>
                pendingEvents{};
            std::size_t pendingEventCount = 0;
            for (std::size_t index = 0;
                 index < prepared.sampleCount;
                 ++index) {
                const auto& update = prepared.samples[index];
                WorldPanelRayHit hit{};
                const auto hasHit =
                    prepared.operational &&
                    Hit(target->surface, update, hit);

                int capturedX = -1;
                int capturedY = -1;
                const auto hasCapturedPlane =
                    prepared.operational &&
                    target->surface &&
                    interaction.captured &&
                    IntersectCapturedPlane(
                        *target->surface,
                        update,
                        capturedX,
                        capturedY);

                SpatialPointerProtocol::Sample sample{
                    .active =
                        (update.flags &
                         PRISMA_UI_VR_API::
                             SpatialPointerUpdate_Active) != 0,
                    .backendReady = prepared.operational,
                    .primaryDown =
                        (update.buttonLevels &
                         PRISMA_UI_VR_API::
                             SpatialPointerButton_Primary) != 0,
                    .inside = hasHit,
                    .hitPixelX = hasHit ? hit.pixelX : -1,
                    .hitPixelY = hasHit ? hit.pixelY : -1,
                    .hasCapturedPlane = hasCapturedPlane,
                    .capturedPixelX = capturedX,
                    .capturedPixelY = capturedY,
                    .scrollDeltaX = update.scrollDeltaX,
                    .scrollDeltaY = update.scrollDeltaY,
                    .sequence = update.sequence,
                    .pixelWidth =
                        target->surface ?
                            target->surface->pixelWidth :
                            0,
                    .pixelHeight =
                        target->surface ?
                            target->surface->pixelHeight :
                            0
                };

                auto next = interaction;
                SpatialPointerProtocol::EventBuffer events;
                const auto result =
                    SpatialPointerProtocol::Apply(
                        next,
                        sample,
                        events);
                if (!result.success ||
                    !AppendProtocolEvents(
                        target->view->id,
                        events,
                        pendingEvents,
                        pendingEventCount)) {
                    allApplied = false;
                    break;
                }
                interaction = next;
                appliedSequence = update.sequence;
                if (hasHit) {
                    lastHit = hit;
                    hasLastHit = true;
                } else {
                    hasLastHit = false;
                }
                lastApplyResult =
                    result.disposition ==
                            SpatialPointerProtocol::
                                ApplyDisposition::Applied ?
                        SpatialResult::Ok :
                        SpatialResult::NotReady;
            }

            if (!allApplied ||
                !InputHandler::EnqueueSpatialPointerEvents(
                    std::span(pendingEvents).
                        first(pendingEventCount))) {
                state.lastApplyResult =
                    SpatialResult::ResourceLimit;
                return;
            }

            state.interaction = interaction;
            state.appliedSequence = appliedSequence;
            state.lastApplyResult = lastApplyResult;
            state.active = prepared.routingSample;
            state.hasActive =
                !prepared.timedOut &&
                (prepared.routingSample.flags &
                 PRISMA_UI_VR_API::
                     SpatialPointerUpdate_Active) != 0;
            state.backendReady = prepared.operational;
            state.routed =
                prepared.routingSample.pointerSourceId != 0;
            state.hasHit = hasLastHit;
            state.hitDistance =
                hasLastHit ? lastHit.distance : 0.0f;
            state.hitUv[0] = hasLastHit ? lastHit.u : 0.0f;
            state.hitUv[1] = hasLastHit ? lastHit.v : 0.0f;

            if (hasLastHit && target->surface) {
                target->reticle.visible = true;
                target->reticle.center = lastHit.position;
                target->reticle.right = target->surface->right;
                target->reticle.up = target->surface->up;
                const auto extent = (std::min)(
                    target->surface->physicalWidth,
                    target->surface->physicalHeight);
                target->reticle.physicalWidth =
                    std::clamp(extent * 0.02f, 0.5f, 4.0f);
                target->reticle.physicalHeight =
                    target->reticle.physicalWidth;
            }
        }
    }

    SpatialResult SubmitUpdate(
        Core::PrismaViewId viewId,
        const SpatialPointerUpdateV1* update) noexcept
    {
        if (!update) {
            return SpatialResult::InvalidArgument;
        }
        if (update->structSize < sizeof(*update)) {
            return SpatialResult::InvalidStructSize;
        }
        if (Core::IsShuttingDown()) {
            return SpatialResult::ShuttingDown;
        }

        const auto view = Core::FindView(viewId);
        if (!view) {
            return SpatialResult::InvalidView;
        }
        SpatialPointerUpdateV1 normalized = *update;
        if (!ValidateAndNormalize(normalized)) {
            return SpatialResult::InvalidArgument;
        }

        try {
            std::lock_guard lock(view->spatialPointerMutex);
            auto& state = view->spatialPointer;
            if (normalized.sequence <= state.acceptedSequence) {
                return SpatialResult::StaleSequence;
            }

            auto replaced = false;
            if (state.sampleCount == state.samples.size()) {
                const auto last =
                    (state.sampleHead + state.sampleCount - 1) %
                    state.samples.size();
                if (!SpatialPointerProtocol::MayCoalesce(
                        QueueKey(state.samples[last]),
                        QueueKey(normalized),
                        PRISMA_UI_VR_API::
                            SpatialPointerUpdate_Active)) {
                    return SpatialResult::ResourceLimit;
                }
                state.samples[last] = normalized;
                ++state.replacedPendingUpdateCount;
                replaced = true;
            } else {
                const auto tail =
                    (state.sampleHead + state.sampleCount) %
                    state.samples.size();
                state.samples[tail] = normalized;
                ++state.sampleCount;
            }

            state.acceptedSequence = normalized.sequence;
            state.lastSubmissionTime =
                std::chrono::steady_clock::now();
            state.lastApplyResult = SpatialResult::NotReady;
            return replaced ?
                SpatialResult::PendingUpdateReplaced :
                SpatialResult::Ok;
        } catch (...) {
            return SpatialResult::InternalError;
        }
    }

    SpatialResult Cancel(Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view) {
            return SpatialResult::InvalidView;
        }
        return CancelForView(view, false) ?
            SpatialResult::Ok :
            SpatialResult::ResourceLimit;
    }

    SpatialResult GetState(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::SpatialPointerStateV1* outState) noexcept
    {
        using namespace PRISMA_UI_VR_API;

        if (!outState) {
            return SpatialResult::InvalidArgument;
        }
        if (outState->structSize < sizeof(*outState)) {
            return SpatialResult::InvalidStructSize;
        }
        if (Core::IsShuttingDown()) {
            return SpatialResult::ShuttingDown;
        }
        const auto view = Core::FindView(viewId);
        if (!view) {
            return SpatialResult::InvalidView;
        }

        try {
            SpatialPointerStateV1 result{};
            result.structSize = sizeof(result);
            std::lock_guard lock(view->spatialPointerMutex);
            const auto& state = view->spatialPointer;
            result.lastApplyResult =
                static_cast<std::int32_t>(
                    state.lastApplyResult);
            result.buttonLevels =
                state.hasActive ?
                    state.active.buttonLevels :
                    0;
            result.acceptedSequence =
                state.acceptedSequence;
            result.appliedSequence =
                state.appliedSequence;
            result.hitDistance = state.hitDistance;
            result.hitUv[0] = state.hitUv[0];
            result.hitUv[1] = state.hitUv[1];
            result.pixelX = state.interaction.lastPixelX;
            result.pixelY = state.interaction.lastPixelY;
            result.replacedPendingUpdateCount =
                state.replacedPendingUpdateCount;
            result.pointerSourceId =
                state.hasActive ?
                    state.active.pointerSourceId :
                    0;
            if (state.hasActive &&
                (state.active.flags &
                 SpatialPointerUpdate_Active) != 0) {
                result.stateFlags |=
                    SpatialPointerState_Active;
            }
            if (state.sampleCount > 0) {
                result.stateFlags |=
                    SpatialPointerState_Pending;
            }
            if (state.appliedSequence > 0) {
                result.stateFlags |=
                    SpatialPointerState_Applied;
            }
            if (state.hasHit) {
                result.stateFlags |=
                    SpatialPointerState_Hit;
            }
            if (state.interaction.captured) {
                result.stateFlags |=
                    SpatialPointerState_Captured;
            }
            if (state.backendReady) {
                result.stateFlags |=
                    SpatialPointerState_BackendReady;
            }
            if (state.routed) {
                result.stateFlags |=
                    SpatialPointerState_Routed;
            }
            *outState = result;
            return SpatialResult::Ok;
        } catch (...) {
            return SpatialResult::InternalError;
        }
    }

    void ProcessFrameBatch(
        std::span<FrameTarget> targets) noexcept
    {
        constexpr auto capacity =
            Core::kMaximumFrameworkViews;
        static_assert(
            capacity >=
            PrismaUI::SpatialPresentation::kMaximumSpatialViews);

        // DrawViews passes every visible view, not only views with an active
        // spatial presentation. Keep bounded scratch storage for the complete
        // framework view set so later world panels cannot be skipped.
        thread_local std::array<Prepared, capacity> prepared{};
        thread_local std::array<
            SpatialPointerRouter::Candidate,
            capacity>
            candidates{};
        thread_local std::array<std::uint8_t, capacity> winners{};

        const auto count = (std::min)(
            targets.size(),
            prepared.size());
        for (std::size_t index = 0; index < count; ++index) {
            candidates[index] = {};
            Prepare(targets[index], prepared[index]);
            const auto& item = prepared[index];
            if (!item.target || !item.target->view ||
                !item.hasRoutingSample) {
                continue;
            }
            candidates[index] = {
                .viewId = item.target->view->id,
                .sourceId =
                    item.routingSample.pointerSourceId,
                .drawOrder = item.target->drawOrder,
                .active =
                    (item.routingSample.flags &
                     PRISMA_UI_VR_API::
                         SpatialPointerUpdate_Active) != 0,
                .operational = item.operational,
                .captured = item.captured,
                .hit = item.routingHasHit,
                .hitDistance =
                    item.routingHasHit ?
                        item.routingHit.distance :
                        0.0f
            };
        }

        (void)SpatialPointerRouter::SelectWinners(
            std::span(candidates).first(count),
            std::span(winners).first(count));
        for (std::size_t index = 0; index < count; ++index) {
            const auto perView =
                candidates[index].sourceId == 0;
            ProcessPrepared(
                prepared[index],
                perView || winners[index] != 0);
        }
    }

    bool CancelForView(
        const std::shared_ptr<Core::PrismaView>& view,
        bool dispatchEvents) noexcept
    {
        if (!view) {
            return false;
        }
        if (ApplyCancel(view, true, true)) {
            if (!dispatchEvents) {
                return true;
            }
            return InputHandler::
                ScheduleSpatialPointerEventProcessing();
        }
        if (!dispatchEvents) {
            return false;
        }

        auto& worker = Core::GetRuntime().worker;
        if (worker.IsWorkerThread()) {
            InputHandler::ProcessEvents();
            if (!ApplyCancel(view, true, true)) {
                return false;
            }
            InputHandler::ProcessEvents();
            return true;
        }
        return worker.TryPost(
            SingleThreadExecutor::Priority::FRAME_CRITICAL,
            [view] {
                InputHandler::ProcessEvents();
                if (!ApplyCancel(view, true, true)) {
                    logger::error(
                        "View [{}] could not enqueue its forced pointer release",
                        view->id);
                    return;
                }
                InputHandler::ProcessEvents();
            });
    }

    void HandleBackendUnavailable() noexcept
    {
        std::vector<std::shared_ptr<Core::PrismaView>> snapshot;
        auto& runtime = Core::GetRuntime();
        {
            std::shared_lock lock(runtime.viewsMutex);
            snapshot.reserve(runtime.views.size());
            for (const auto& [id, view] : runtime.views) {
                (void)id;
                if (view) {
                    snapshot.push_back(view);
                }
            }
        }
        for (const auto& view : snapshot) {
            (void)ApplyCancel(view, true, true);
            std::lock_guard lock(view->spatialPointerMutex);
            view->spatialPointer.lastApplyResult =
                SpatialResult::NotReady;
        }
    }

    void Shutdown() noexcept
    {
        HandleBackendUnavailable();
        InputHandler::FlushSpatialPointerEvents();
    }
}
