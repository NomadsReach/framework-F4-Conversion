#include "PCH.h"

#include "PrismaUI/SpatialPresentation.h"

#include "PrismaUI/SceneDepthCapture.h"
#include "PrismaUI/ViewManager.h"

namespace PrismaUI::SpatialPresentation
{
    namespace
    {
        constexpr std::uint32_t apiFlavorF4Vr = 0x52563446;
        constexpr std::uint32_t spatialRevision = 1;
        constexpr float maximumWorldMagnitude = 1.0e8f;
        constexpr float maximumPhysicalDimension = 1.0e6f;
        constexpr float minimumQuaternionNormSquared = 1.0e-8f;

        std::mutex submissionMutex;

        [[nodiscard]] std::uint32_t SupportedUpdateFlags() noexcept
        {
            return SceneDepthCapture::IsInstalled() ?
                PRISMA_UI_VR_API::SpatialUpdate_SceneDepthOcclusion :
                0u;
        }

        [[nodiscard]] bool IsFiniteAndBounded(
            float value,
            float maximum) noexcept
        {
            return std::isfinite(value) && std::fabs(value) <= maximum;
        }

        [[nodiscard]] bool IsWorldMode(
            PRISMA_UI_VR_API::SpatialPresentationMode mode) noexcept
        {
            return mode ==
                       PRISMA_UI_VR_API::SpatialPresentationMode::WorldBillboard ||
                   mode ==
                       PRISMA_UI_VR_API::SpatialPresentationMode::WorldQuad;
        }

        [[nodiscard]] bool ValidateAndNormalize(
            PRISMA_UI_VR_API::SpatialUpdateV1& update) noexcept
        {
            using enum PRISMA_UI_VR_API::SpatialPresentationMode;

            if (update.coordinateSpace !=
                    PRISMA_UI_VR_API::SpatialCoordinateSpace::GameWorld ||
                (update.flags & ~SupportedUpdateFlags()) != 0 ||
                update.sequence == 0 ||
                update.dimensions.pixelWidth == 0 ||
                update.dimensions.pixelHeight == 0 ||
                update.dimensions.pixelWidth > kMaximumPixelDimension ||
                update.dimensions.pixelHeight > kMaximumPixelDimension ||
                std::any_of(
                    std::begin(update.reserved),
                    std::end(update.reserved),
                    [](std::uint32_t value) { return value != 0; })) {
                return false;
            }

            if (update.presentationMode == HeadLockedQuad) {
                if ((update.flags &
                     PRISMA_UI_VR_API::
                         SpatialUpdate_SceneDepthOcclusion) != 0) {
                    return false;
                }
                update.pose = {};
                update.pose.orientation[3] = 1.0f;
                update.dimensions.physicalWidth = 0.0f;
                update.dimensions.physicalHeight = 0.0f;
                return true;
            }

            if (update.presentationMode != WorldBillboard &&
                update.presentationMode != WorldQuad) {
                return false;
            }

            for (const auto component : update.pose.position) {
                if (!IsFiniteAndBounded(
                        component,
                        maximumWorldMagnitude)) {
                    return false;
                }
            }
            if (!IsFiniteAndBounded(
                    update.dimensions.physicalWidth,
                    maximumPhysicalDimension) ||
                !IsFiniteAndBounded(
                    update.dimensions.physicalHeight,
                    maximumPhysicalDimension) ||
                update.dimensions.physicalWidth <= 0.0f ||
                update.dimensions.physicalHeight <= 0.0f) {
                return false;
            }

            if (update.presentationMode == WorldBillboard) {
                update.pose.orientation[0] = 0.0f;
                update.pose.orientation[1] = 0.0f;
                update.pose.orientation[2] = 0.0f;
                update.pose.orientation[3] = 1.0f;
                return true;
            }

            float normSquared = 0.0f;
            for (const auto component : update.pose.orientation) {
                if (!IsFiniteAndBounded(
                        component,
                        maximumWorldMagnitude)) {
                    return false;
                }
                normSquared += component * component;
            }
            if (!std::isfinite(normSquared) ||
                normSquared < minimumQuaternionNormSquared) {
                return false;
            }
            const auto inverseNorm = 1.0f / std::sqrt(normSquared);
            for (auto& component : update.pose.orientation) {
                component *= inverseNorm;
            }
            return true;
        }

        [[nodiscard]] const PRISMA_UI_VR_API::SpatialUpdateV1*
            LatestUpdate(const Core::SpatialRuntimeState& state) noexcept
        {
            if (state.hasPending) {
                return &state.pending;
            }
            if (state.hasActive) {
                return &state.active;
            }
            return nullptr;
        }

        [[nodiscard]] bool FitsBudget(
            const std::shared_ptr<Core::PrismaView>& target,
            const PRISMA_UI_VR_API::SpatialUpdateV1& proposal) noexcept
        {
            try {
                std::vector<std::shared_ptr<Core::PrismaView>> snapshot;
                auto& runtime = Core::GetRuntime();
                {
                    std::shared_lock lock(runtime.viewsMutex);
                    snapshot.reserve(runtime.views.size());
                    for (const auto& [id, view] : runtime.views) {
                        if (view &&
                            !view->destroying.load(
                                std::memory_order_acquire)) {
                            snapshot.push_back(view);
                        }
                    }
                }

                std::uint64_t pixels = 0;
                std::uint32_t count = 0;
                bool targetCounted = false;
                for (const auto& view : snapshot) {
                    std::lock_guard lock(view->spatialMutex);
                    const auto* update =
                        view == target ?
                            &proposal :
                            LatestUpdate(view->spatial);
                    if (!update) {
                        continue;
                    }
                    ++count;
                    targetCounted = targetCounted || view == target;
                    pixels +=
                        static_cast<std::uint64_t>(
                            update->dimensions.pixelWidth) *
                        update->dimensions.pixelHeight;
                    if (pixels > kMaximumAggregateSpatialPixels) {
                        return false;
                    }
                }

                if (!targetCounted) {
                    ++count;
                    pixels +=
                        static_cast<std::uint64_t>(
                            proposal.dimensions.pixelWidth) *
                        proposal.dimensions.pixelHeight;
                }
                return count <= kMaximumSpatialViews &&
                       pixels <= kMaximumAggregateSpatialPixels;
            } catch (...) {
                return false;
            }
        }

        [[nodiscard]] PRISMA_UI_VR_API::SpatialResult Accept(
            const std::shared_ptr<Core::PrismaView>& view,
            const PRISMA_UI_VR_API::SpatialUpdateV1& update) noexcept
        {
            using PRISMA_UI_VR_API::SpatialResult;

            if (!FitsBudget(view, update)) {
                return SpatialResult::ResourceLimit;
            }

            std::lock_guard lock(view->spatialMutex);
            if (view->destroying.load(std::memory_order_acquire)) {
                return SpatialResult::InvalidView;
            }
            if (update.sequence <= view->spatial.acceptedSequence) {
                return SpatialResult::StaleSequence;
            }

            const auto replaced = view->spatial.hasPending;
            if (replaced) {
                ++view->spatial.replacedPendingUpdateCount;
            }
            view->spatial.pending = update;
            view->spatial.hasPending = true;
            view->spatial.acceptedSequence = update.sequence;
            view->spatial.lastApplyResult = SpatialResult::NotReady;
            return replaced ?
                SpatialResult::PendingUpdateReplaced :
                SpatialResult::Ok;
        }

        void RevokeScreenFocus(
            const std::shared_ptr<Core::PrismaView>& view) noexcept
        {
            ViewManager::CancelDeferredFocus(view);
            if (view->focused.load(std::memory_order_acquire) ||
                view->focusRequestPending.load(
                    std::memory_order_acquire)) {
                ViewManager::Unfocus(view->id);
            }
        }
    }

    PRISMA_UI_VR_API::SpatialResult GetCapabilities(
        PRISMA_UI_VR_API::SpatialCapabilitiesV1* outCapabilities) noexcept
    {
        using namespace PRISMA_UI_VR_API;

        if (!outCapabilities) {
            return SpatialResult::InvalidArgument;
        }
        if (outCapabilities->structSize < sizeof(*outCapabilities)) {
            return SpatialResult::InvalidStructSize;
        }
        if (Core::IsShuttingDown()) {
            return SpatialResult::ShuttingDown;
        }

        SpatialCapabilitiesV1 capabilities{};
        capabilities.structSize = sizeof(capabilities);
        capabilities.apiFlavor = apiFlavorF4Vr;
        capabilities.spatialRevision = spatialRevision;
        capabilities.featureBits =
            SpatialFeature_FullPose |
            SpatialFeature_IndependentDimensions |
            SpatialFeature_LatestOnlyUpdates |
            SpatialFeature_AppliedSequenceQuery |
            SpatialFeature_StereoCorrectProjection |
            SpatialFeature_CompositorOverlay |
            SpatialFeature_GpuRendering |
            SpatialFeature_RefreshUpTo120Hz |
            SpatialFeature_NativeNetworkPolicy |
            SpatialFeature_WorldPointerInput |
            SpatialFeature_CentralPointerRouting;
        if (SceneDepthCapture::IsInstalled()) {
            capabilities.featureBits |=
                SpatialFeature_SceneDepthOcclusion;
        }
        capabilities.coordinateSpaceMask =
            1ull << static_cast<std::uint32_t>(
                SpatialCoordinateSpace::GameWorld);
        capabilities.presentationModeMask =
            (1ull << static_cast<std::uint32_t>(
                 SpatialPresentationMode::HeadLockedQuad)) |
            (1ull << static_cast<std::uint32_t>(
                 SpatialPresentationMode::WorldBillboard)) |
            (1ull << static_cast<std::uint32_t>(
                 SpatialPresentationMode::WorldQuad));
        capabilities.supportedUpdateFlags = SupportedUpdateFlags();
        capabilities.maxPixelWidth = kMaximumPixelDimension;
        capabilities.maxPixelHeight = kMaximumPixelDimension;
        capabilities.maxSpatialViews = kMaximumSpatialViews;
        capabilities.maxAggregateSpatialPixels =
            kMaximumAggregateSpatialPixels;
        capabilities.maxRefreshRateHz = 120;
        capabilities.maxAbsoluteWorldPosition = maximumWorldMagnitude;
        capabilities.maxPhysicalDimension = maximumPhysicalDimension;
        capabilities.minQuaternionNormSquared =
            minimumQuaternionNormSquared;
        *outCapabilities = capabilities;
        return SpatialResult::Ok;
    }

    PRISMA_UI_VR_API::SpatialResult SubmitUpdate(
        Core::PrismaViewId viewId,
        const PRISMA_UI_VR_API::SpatialUpdateV1* update) noexcept
    {
        using namespace PRISMA_UI_VR_API;

        if (!update) {
            return SpatialResult::InvalidArgument;
        }
        if (update->structSize < sizeof(*update)) {
            return SpatialResult::InvalidStructSize;
        }
        if (Core::IsShuttingDown()) {
            return SpatialResult::ShuttingDown;
        }

        if (update->coordinateSpace !=
                SpatialCoordinateSpace::GameWorld ||
            (update->flags & ~SupportedUpdateFlags()) != 0 ||
            (update->presentationMode ==
                 SpatialPresentationMode::HeadLockedQuad &&
             (update->flags &
              SpatialUpdate_SceneDepthOcclusion) != 0) ||
            (update->presentationMode !=
                 SpatialPresentationMode::HeadLockedQuad &&
             update->presentationMode !=
                 SpatialPresentationMode::WorldBillboard &&
             update->presentationMode !=
                 SpatialPresentationMode::WorldQuad)) {
            return SpatialResult::Unsupported;
        }

        auto view = Core::FindView(viewId);
        if (!view) {
            return SpatialResult::InvalidView;
        }
        if (!Core::IsRenderBackendOperational()) {
            return SpatialResult::NotReady;
        }

        auto normalized = *update;
        normalized.structSize = sizeof(normalized);
        if (!ValidateAndNormalize(normalized)) {
            return SpatialResult::InvalidArgument;
        }

        try {
            std::lock_guard submissionLock(submissionMutex);
            const auto result = Accept(view, normalized);
            if (result == SpatialResult::Ok ||
                result == SpatialResult::PendingUpdateReplaced) {
                if (IsWorldMode(normalized.presentationMode)) {
                    RevokeScreenFocus(view);
                }
                if ((normalized.flags &
                     SpatialUpdate_SceneDepthOcclusion) != 0) {
                    SceneDepthCapture::SetCaptureRequested(true);
                }
            }
            return result;
        } catch (...) {
            return SpatialResult::InternalError;
        }
    }

    PRISMA_UI_VR_API::SpatialResult GetState(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::SpatialStateV1* outState) noexcept
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

        auto view = Core::FindView(viewId);
        if (!view) {
            return SpatialResult::InvalidView;
        }

        try {
            SpatialStateV1 state{};
            state.structSize = sizeof(state);
            const auto backendOperational =
                Core::IsRenderBackendOperational();

            std::lock_guard lock(view->spatialMutex);
            state.acceptedSequence = view->spatial.acceptedSequence;
            state.appliedSequence = view->spatial.appliedSequence;
            state.lastApplyResult = static_cast<std::int32_t>(
                backendOperational ?
                    view->spatial.lastApplyResult :
                    SpatialResult::NotReady);
            state.replacedPendingUpdateCount =
                view->spatial.replacedPendingUpdateCount;

            if (state.acceptedSequence != state.appliedSequence) {
                state.stateFlags |= SpatialState_Pending;
            }
            if (view->spatial.hasApplied) {
                const auto& applied = view->spatial.applied;
                state.coordinateSpace = applied.coordinateSpace;
                state.presentationMode = applied.presentationMode;
                state.appliedPixelWidth =
                    applied.dimensions.pixelWidth;
                state.appliedPixelHeight =
                    applied.dimensions.pixelHeight;
                state.appliedPhysicalWidth =
                    applied.dimensions.physicalWidth;
                state.appliedPhysicalHeight =
                    applied.dimensions.physicalHeight;
                state.appliedPose = applied.pose;
                state.stateFlags |= SpatialState_Applied;
                if (IsWorldMode(applied.presentationMode)) {
                    state.stateFlags |= SpatialState_Enabled;
                }
                if ((applied.flags &
                     SpatialUpdate_SceneDepthOcclusion) != 0) {
                    state.stateFlags |=
                        SpatialState_SceneDepthOcclusion;
                }
            }
            if (view->hidden.load(std::memory_order_acquire)) {
                state.stateFlags |= SpatialState_ViewHidden;
            }
            if (backendOperational && view->spatial.backendReady) {
                state.stateFlags |= SpatialState_BackendReady;
            }
            *outState = state;
            return SpatialResult::Ok;
        } catch (...) {
            return SpatialResult::InternalError;
        }
    }

    bool BeginFrame(
        const std::shared_ptr<Core::PrismaView>& view,
        PRISMA_UI_VR_API::SpatialUpdateV1& outActive) noexcept
    {
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            std::lock_guard lock(view->spatialMutex);
            if (view->destroying.load(std::memory_order_acquire)) {
                return false;
            }
            if (view->spatial.hasPending) {
                view->spatial.active = view->spatial.pending;
                view->spatial.hasActive = true;
                view->spatial.hasPending = false;
                view->spatial.backendReady = false;
                view->spatial.lastApplyResult =
                    PRISMA_UI_VR_API::SpatialResult::NotReady;
            }
            if (!view->spatial.hasActive) {
                return false;
            }
            outActive = view->spatial.active;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool HasRenderOnlyWorldPresentation(
        const std::shared_ptr<Core::PrismaView>& view) noexcept
    {
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            std::lock_guard lock(view->spatialMutex);
            return
                (view->spatial.hasPending &&
                 IsWorldMode(view->spatial.pending.presentationMode)) ||
                (view->spatial.hasActive &&
                 IsWorldMode(view->spatial.active.presentationMode)) ||
                (view->spatial.hasApplied &&
                 IsWorldMode(view->spatial.applied.presentationMode));
        } catch (...) {
            return true;
        }
    }

    bool IsTransitioningToHeadLocked(
        const std::shared_ptr<Core::PrismaView>& view) noexcept
    {
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            std::lock_guard lock(view->spatialMutex);
            const auto* newest =
                view->spatial.hasPending ?
                    &view->spatial.pending :
                view->spatial.hasActive ?
                    &view->spatial.active :
                    nullptr;
            if (!newest ||
                newest->presentationMode !=
                    PRISMA_UI_VR_API::SpatialPresentationMode::
                        HeadLockedQuad) {
                return false;
            }
            return
                (view->spatial.hasActive &&
                 IsWorldMode(view->spatial.active.presentationMode)) ||
                (view->spatial.hasApplied &&
                 IsWorldMode(view->spatial.applied.presentationMode));
        } catch (...) {
            return false;
        }
    }

    std::pair<std::uint32_t, std::uint32_t> GetDesiredPixelSize(
        const std::shared_ptr<Core::PrismaView>& view,
        std::uint32_t fallbackWidth,
        std::uint32_t fallbackHeight) noexcept
    {
        if (!view) {
            return {fallbackWidth, fallbackHeight};
        }
        try {
            std::lock_guard lock(view->spatialMutex);
            const auto* update =
                view->spatial.hasPending ?
                    &view->spatial.pending :
                view->spatial.hasActive ?
                    &view->spatial.active :
                    nullptr;
            if (update &&
                update->dimensions.pixelWidth > 0 &&
                update->dimensions.pixelHeight > 0) {
                return {
                    update->dimensions.pixelWidth,
                    update->dimensions.pixelHeight
                };
            }
        } catch (...) {
        }
        return {fallbackWidth, fallbackHeight};
    }

    void MarkApplied(
        const std::shared_ptr<Core::PrismaView>& view,
        const PRISMA_UI_VR_API::SpatialUpdateV1& frameUpdate,
        bool backendReady,
        std::uint32_t renderedPixelWidth,
        std::uint32_t renderedPixelHeight) noexcept
    {
        if (!view) {
            return;
        }

        bool applyDeferredFocus = false;
        try {
            std::lock_guard lock(view->spatialMutex);
            if (view->destroying.load(std::memory_order_acquire) ||
                !view->spatial.hasActive ||
                view->spatial.active.sequence != frameUpdate.sequence) {
                return;
            }

            const auto dimensionsApplied =
                renderedPixelWidth ==
                    frameUpdate.dimensions.pixelWidth &&
                renderedPixelHeight ==
                    frameUpdate.dimensions.pixelHeight;
            if (backendReady && dimensionsApplied) {
                view->spatial.applied = frameUpdate;
                view->spatial.hasApplied = true;
                view->spatial.appliedSequence = frameUpdate.sequence;
                view->spatial.backendReady =
                    !view->spatial.hasPending;
                view->spatial.lastApplyResult =
                    view->spatial.hasPending ?
                        PRISMA_UI_VR_API::SpatialResult::NotReady :
                        PRISMA_UI_VR_API::SpatialResult::Ok;
                applyDeferredFocus =
                    frameUpdate.presentationMode ==
                    PRISMA_UI_VR_API::SpatialPresentationMode::
                        HeadLockedQuad;
            } else {
                view->spatial.backendReady = false;
                view->spatial.lastApplyResult =
                    PRISMA_UI_VR_API::SpatialResult::NotReady;
            }
        } catch (...) {
            return;
        }

        if (applyDeferredFocus) {
            ViewManager::ApplyDeferredFocusIfReady(view);
        }
    }

    void MarkBackendUnavailable(
        const std::shared_ptr<Core::PrismaView>& view) noexcept
    {
        if (!view) {
            return;
        }
        try {
            std::lock_guard lock(view->spatialMutex);
            view->spatial.backendReady = false;
            view->spatial.lastApplyResult =
                PRISMA_UI_VR_API::SpatialResult::NotReady;
        } catch (...) {
        }
    }
}
