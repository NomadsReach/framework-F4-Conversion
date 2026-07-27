#include "Hooks/EngineStereoSubmissionPolicy.h"
#include "PrismaUI/GpuGeometryIndexPolicy.h"
#include "PrismaUI/GpuResourceBudget.h"
#include "PrismaUI/PresentationThreadPolicy.h"
#include "PrismaUI/RenderTargetSamplingPolicy.h"
#include "PrismaUI/SpatialPointerProtocol.h"
#include "PrismaUI/SpatialPointerRouter.h"
#include "PrismaUI/VirtualFilePolicy.h"
#include "PrismaUI/WorldPanelGeometry.h"
#include "PrismaUI_F4_API.h"
#include "PrismaUI_F4VR_API.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace
{
    int failures = 0;

    void Expect(bool condition, std::string_view message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    bool Near(float left, float right, float epsilon = 1.0e-4f)
    {
        return std::fabs(left - right) <= epsilon;
    }

    PRISMA_UI_VR_API::SpatialUpdateV1 MakeWorldQuad()
    {
        PRISMA_UI_VR_API::SpatialUpdateV1 update{};
        update.structSize = sizeof(update);
        update.coordinateSpace =
            PRISMA_UI_VR_API::SpatialCoordinateSpace::GameWorld;
        update.presentationMode =
            PRISMA_UI_VR_API::SpatialPresentationMode::WorldQuad;
        update.sequence = 1;
        update.pose.position[0] = 10.0f;
        update.pose.position[1] = 20.0f;
        update.pose.position[2] = 30.0f;
        update.pose.orientation[3] = 1.0f;
        update.dimensions.pixelWidth = 1000;
        update.dimensions.pixelHeight = 500;
        update.dimensions.physicalWidth = 200.0f;
        update.dimensions.physicalHeight = 100.0f;
        return update;
    }

    void TestApiLayout()
    {
        using PRISMA_UI_API::InterfaceVersion;
        Expect(
            static_cast<std::uint8_t>(InterfaceVersion::V1) == 0 &&
                static_cast<std::uint8_t>(InterfaceVersion::V4) == 3,
            "base interface ordinals remain V1=0 through V4=3");
        Expect(
            static_cast<std::uint8_t>(
                PRISMA_UI_VR_API::InterfaceVersion::V1) == 0,
            "VR extension begins its own version sequence");
        Expect(
            sizeof(PRISMA_UI_VR_API::SpatialUpdateV1) == 96 &&
                sizeof(PRISMA_UI_VR_API::SpatialStateV1) == 96,
            "spatial update/state ABI remains 96 bytes");
        Expect(
            sizeof(PRISMA_UI_VR_API::SpatialPointerUpdateV1) == 96 &&
                sizeof(PRISMA_UI_VR_API::SpatialPointerStateV1) == 96,
            "pointer update/state ABI remains 96 bytes");
        Expect(
            static_cast<std::int32_t>(
                PRISMA_UI_VR_API::SpatialResult::ResourceLimit) == -9,
            "spatial result ordinals remain stable");
    }

    void TestVirtualFilePolicy()
    {
        namespace Policy = PrismaUI::VirtualFilePolicy;

        Expect(
            Policy::IsSafeRelativePath(
                "consumer/panel.html?mode=vr"),
            "ordinary relative view paths remain available");
        Expect(
            !Policy::IsSafeRelativePath(
                "consumer/%2e%2e/secret.html"),
            "encoded relative traversal is rejected");
        Expect(
            Policy::IsSafeFileUrl(
                "file:///views/consumer/panel.html",
                ""),
            "virtual-root file URLs remain available");
        Expect(
            Policy::IsSafeFileUrl(
                "file://localhost/icons/icon.png",
                "localhost"),
            "localhost virtual file URLs remain available");
        Expect(
            !Policy::IsSafeFileUrl(
                "file:///views/%2e%2e/%2e%2e/secret.txt",
                ""),
            "encoded file traversal is rejected");
        Expect(
            !Policy::IsSafeFileUrl(
                "file:///Z:/private/example.txt",
                ""),
            "host filesystem drive paths are rejected");
        Expect(
            !Policy::IsSafeFileUrl(
                "file://remotehost/views/panel.html",
                "remotehost"),
            "remote file authorities are rejected");
    }

    void TestWorldQuadGeometry()
    {
        namespace Geometry = PrismaUI::WorldPanelGeometry;

        const auto update = MakeWorldQuad();
        Geometry::WorldPanelPlacement placement{};
        Expect(
            Geometry::MakePlacement(update, placement),
            "valid oriented world update creates a placement");

        Geometry::WorldPanelSurface surface{};
        Expect(
            Geometry::ResolveSurface(placement, {}, surface),
            "identity quaternion resolves an oriented surface");
        Expect(
            Near(surface.right.x, 1.0f) &&
                Near(surface.up.y, 1.0f) &&
                Near(surface.normal.z, 1.0f),
            "identity quaternion preserves local panel basis");
        Expect(
            Near(surface.corners[0].x, -90.0f) &&
                Near(surface.corners[0].y, 70.0f),
            "top-left corner uses independent physical dimensions");

        Geometry::WorldPanelRayHit centerHit{};
        Expect(
            Geometry::IntersectRay(
                surface,
                {10.0f, 20.0f, 130.0f},
                {0.0f, 0.0f, -2.0f},
                200.0f,
                centerHit),
            "two-sided ray hits the world quad center");
        Expect(
            Near(centerHit.u, 0.5f) &&
                Near(centerHit.v, 0.5f) &&
                centerHit.pixelX == 500 &&
                centerHit.pixelY == 250,
            "center hit maps to the center raster pixel");

        Geometry::WorldPanelRayHit miss{};
        Expect(
            !Geometry::IntersectRay(
                surface,
                {500.0f, 500.0f, 130.0f},
                {0.0f, 0.0f, -1.0f},
                200.0f,
                miss),
            "ray outside physical bounds fails closed");
    }

    void TestBillboardAndValidation()
    {
        namespace Geometry = PrismaUI::WorldPanelGeometry;
        auto update = MakeWorldQuad();
        update.presentationMode =
            PRISMA_UI_VR_API::SpatialPresentationMode::WorldBillboard;

        Geometry::WorldPanelPlacement placement{};
        Expect(
            Geometry::MakePlacement(update, placement),
            "billboard update ignores caller orientation");

        Geometry::WorldPanelSurface surface{};
        Expect(
            Geometry::ResolveSurface(
                placement,
                {10.0f, 20.0f, 130.0f},
                surface),
            "billboard faces the supplied eye midpoint");
        Expect(
            Near(surface.normal.z, 1.0f),
            "billboard normal points toward the eye midpoint");

        update.reserved[2] = 1;
        Expect(
            !Geometry::MakePlacement(update, placement),
            "non-zero reserved fields fail closed");

        update = MakeWorldQuad();
        update.pose.orientation[0] = 0.0f;
        update.pose.orientation[1] = 0.0f;
        update.pose.orientation[2] = 0.0f;
        update.pose.orientation[3] = 0.0f;
        Expect(
            !Geometry::MakePlacement(update, placement),
            "zero quaternion is rejected");
    }

    namespace Pointer = PrismaUI::SpatialPointerProtocol;

    Pointer::Sample ReadyPointer(bool down, int x, int y, std::uint64_t sequence)
    {
        Pointer::Sample sample{};
        sample.active = true;
        sample.backendReady = true;
        sample.primaryDown = down;
        sample.inside = true;
        sample.hitPixelX = x;
        sample.hitPixelY = y;
        sample.sequence = sequence;
        sample.pixelWidth = 1000;
        sample.pixelHeight = 500;
        return sample;
    }

    void TestPointerNeutralGateAndEdges()
    {
        Pointer::State state{};

        Pointer::EventBuffer heldOnEntry{};
        const auto first = Pointer::Apply(
            state,
            ReadyPointer(true, 100, 100, 1),
            heldOnEntry);
        Expect(
            first.success &&
                !state.captured &&
                heldOnEntry.count == 1,
            "held button on stream entry cannot synthesize a down edge");

        Pointer::EventBuffer neutral{};
        Expect(
            Pointer::Apply(
                state,
                ReadyPointer(false, 101, 100, 2),
                neutral)
                .success &&
                !state.requiresPrimaryRelease,
            "routed neutral sample opens the capture gate");

        Pointer::EventBuffer press{};
        Expect(
            Pointer::Apply(
                state,
                ReadyPointer(true, 102, 100, 3),
                press)
                .success &&
                state.captured,
            "fresh rising edge captures the panel");
        Expect(
            press.count == 2 &&
                press.events[0].kind == Pointer::EventKind::Move &&
                press.events[1].kind == Pointer::EventKind::Down,
            "pointer movement precedes the down edge");

        Pointer::EventBuffer release{};
        Expect(
            Pointer::Apply(
                state,
                ReadyPointer(false, 103, 100, 4),
                release)
                .success &&
                !state.captured,
            "falling edge releases capture");
        Expect(
            release.count == 2 &&
                release.events[0].buttonDown &&
                release.events[1].kind == Pointer::EventKind::Up,
            "move retains held state until the up event");
    }

    void TestPointerCancellationAndMotion()
    {
        Pointer::State state{};
        Pointer::EventBuffer neutral{};
        (void)Pointer::Apply(state, ReadyPointer(false, 200, 200, 10), neutral);
        Pointer::EventBuffer press{};
        (void)Pointer::Apply(state, ReadyPointer(true, 200, 200, 11), press);

        Pointer::EventBuffer jitter{};
        Expect(
            Pointer::Apply(state, ReadyPointer(true, 215, 205, 12), jitter)
                .success &&
                jitter.events[0].x == 200 &&
                jitter.events[0].y == 200,
            "sub-slop trigger motion stays on the press point");

        Pointer::EventBuffer drag{};
        Expect(
            Pointer::Apply(state, ReadyPointer(true, 400, 200, 13), drag)
                .success &&
                state.dragStarted &&
                drag.events[0].x == 400,
            "large intentional motion exits click slop");

        Pointer::EventBuffer cancel{};
        Expect(
            Pointer::Cancel(state, cancel, true, true) &&
                cancel.count == 2 &&
                cancel.events[0].kind == Pointer::EventKind::Move &&
                cancel.events[1].kind == Pointer::EventKind::Up,
            "capture cancellation emits forced leave then up");
        Expect(
            state.requiresPrimaryRelease,
            "cancellation restores the neutral gate");

        constexpr auto active =
            PRISMA_UI_VR_API::SpatialPointerUpdate_Active;
        Expect(
            !Pointer::MayCoalesce(
                {active, 0, 0, 0, 1},
                {active, 1, 0, 0, 1},
                active),
            "queue coalescing preserves button edges");
        Expect(
            Pointer::MayCoalesce(
                {active, 1, 0, 0, 1},
                {active, 1, 0, 0, 1},
                active),
            "equivalent movement-only samples may coalesce");
    }

    template <std::size_t Size>
    std::array<std::uint8_t, Size> Route(
        const std::array<PrismaUI::SpatialPointerRouter::Candidate, Size>&
            candidates)
    {
        std::array<std::uint8_t, Size> winners{};
        Expect(
            PrismaUI::SpatialPointerRouter::SelectWinners(
                candidates,
                winners),
            "router accepts equally sized candidate and output spans");
        return winners;
    }

    void TestPointerRouting()
    {
        using Candidate = PrismaUI::SpatialPointerRouter::Candidate;

        const std::array nearestCandidates{
            Candidate{10, 1, 0, true, true, false, true, 8.0f},
            Candidate{20, 1, 0, true, true, false, true, 2.0f},
            Candidate{30, 2, 0, true, true, false, true, 4.0f}
        };
        const auto nearest = Route(nearestCandidates);
        Expect(
            nearest[0] == 0 &&
                nearest[1] == 1 &&
                nearest[2] == 1,
            "each physical source selects its nearest panel");

        const std::array capturedCandidates{
            Candidate{10, 1, 0, true, true, true, false, 0.0f},
            Candidate{20, 1, 100, true, true, false, true, 1.0f}
        };
        const auto captured = Route(capturedCandidates);
        Expect(
            captured[0] == 1 && captured[1] == 0,
            "capture retains source ownership through off-panel drag");

        const std::array legacyCandidates{
            Candidate{10, 0, 0, false, false, false, false, 0.0f},
            Candidate{20, 0, 0, true, true, false, false, 0.0f}
        };
        const auto perView = Route(legacyCandidates);
        Expect(
            perView[0] == 1 && perView[1] == 1,
            "source zero preserves independent per-view routing");
    }

    void TestRenderAndHookPolicies()
    {
        const auto capturedCompositorAllocation =
            PrismaUI::GpuResourceBudget::
                PlanRenderTargetAllocation(
                    12354808,
                    2734,
                    1686);
        Expect(
            capturedCompositorAllocation.valid &&
                capturedCompositorAllocation.pixels ==
                    4609524,
            "captured VR compositor render buffer fits the bounded GPU working set");

        const auto finalBudgetPixel =
            PrismaUI::GpuResourceBudget::
                PlanRenderTargetAllocation(
                    PrismaUI::GpuResourceBudget::
                            kMaximumRenderTargetPixels -
                        1,
                    1,
                    1);
        Expect(
            finalBudgetPixel.valid,
            "render-target budget accepts its final bounded pixel");
        Expect(
            !PrismaUI::GpuResourceBudget::
                 PlanRenderTargetAllocation(
                     PrismaUI::GpuResourceBudget::
                         kMaximumRenderTargetPixels,
                     1,
                     1)
                 .valid,
            "render-target budget rejects allocations beyond its hard ceiling");

        const auto clean =
            PrismaUI::RenderTargetSamplingPolicy::MakePlan(
                false,
                false,
                false);
        Expect(
            clean.valid &&
                !clean.resolveBeforeSampling &&
                !clean.suspendBoundTarget,
            "single-sample target needs no resolve");

        const auto dirty =
            PrismaUI::RenderTargetSamplingPolicy::MakePlan(
                true,
                true,
                false);
        Expect(
            dirty.valid &&
                dirty.resolveBeforeSampling &&
                dirty.suspendBoundTarget,
            "dirty bound MSAA target is suspended and resolved");

        Expect(
            !PrismaUI::RenderTargetSamplingPolicy::MakePlan(
                 false,
                 false,
                 true)
                 .valid,
            "physical read/write alias fails closed");

        constexpr std::uintptr_t callsite = 0x1000;
        constexpr std::int32_t displacement = -0x100;
        static_assert(
            Hooks::EngineStereoSubmissionPolicy::RelativeCallTarget(
                callsite,
                displacement) == 0xF05);
        Expect(
            Hooks::EngineStereoSubmissionPolicy::kFullSubmitBoundary[
                Hooks::EngineStereoSubmissionPolicy::kCallsitePrefixSize] ==
                0xE8,
            "verified full submission boundary contains the call opcode");
    }

    void TestGeometryIndexPolicy()
    {
        const std::array<std::uint32_t, 4> indices{
            0,
            1,
            99,
            2
        };
        const auto bytes = std::as_bytes(std::span(indices));
        std::vector<std::uint32_t> invalidPrefix;
        Expect(
            PrismaUI::GpuGeometryIndexPolicy::
                BuildInvalidIndexPrefix(
                    invalidPrefix,
                    bytes.data(),
                    bytes.size(),
                    3),
            "geometry index validation accepts retained buffer capacity");
        Expect(
            PrismaUI::GpuGeometryIndexPolicy::
                DrawRangeReferencesExistingVertices(
                    invalidPrefix,
                    bytes.size(),
                    0,
                    2),
            "draw accepts a valid range before unused invalid capacity");
        Expect(
            !PrismaUI::GpuGeometryIndexPolicy::
                 DrawRangeReferencesExistingVertices(
                     invalidPrefix,
                     bytes.size(),
                     2,
                     1),
            "draw rejects a consumed index beyond the vertex buffer");
        Expect(
            PrismaUI::GpuGeometryIndexPolicy::
                DrawRangeReferencesExistingVertices(
                    invalidPrefix,
                    bytes.size(),
                    3,
                    1),
            "draw accepts valid indices after unused invalid capacity");
        Expect(
            !PrismaUI::GpuGeometryIndexPolicy::DrawRangeIsValid(
                invalidPrefix,
                bytes.size(),
                4,
                1),
            "draw rejects a range beyond the staged index buffer");
    }

    void TestPresentationThreadPolicy()
    {
        PrismaUI::PresentationThreadPolicy::
            SerializedTracker<std::uint32_t>
                tracker;

        const auto first = tracker.Observe(11);
        Expect(
            first.first &&
                !first.migrated &&
                first.migrationCount == 0,
            "first serialized presentation caller establishes diagnostics");

        const auto same = tracker.Observe(11);
        Expect(
            !same.first &&
                !same.migrated &&
                same.migrationCount == 0,
            "same presentation caller does not report migration");

        const auto migrated = tracker.Observe(29);
        Expect(
            migrated.migrated &&
                migrated.migrationCount == 1,
            "serialized FO4VR presentation admits and records thread migration");

        const auto migratedAgain = tracker.Observe(11);
        Expect(
            migratedAgain.migrated &&
                migratedAgain.migrationCount == 2,
            "presentation remains operational across repeated engine-thread migration");
    }
}

int main()
{
    TestApiLayout();
    TestVirtualFilePolicy();
    TestWorldQuadGeometry();
    TestBillboardAndValidation();
    TestPointerNeutralGateAndEdges();
    TestPointerCancellationAndMotion();
    TestPointerRouting();
    TestRenderAndHookPolicies();
    TestGeometryIndexPolicy();
    TestPresentationThreadPolicy();

    if (failures != 0) {
        std::cerr << failures << " deterministic assertion(s) failed\n";
        return 1;
    }
    std::cout << "PrismaUI FO4VR deterministic tests passed\n";
    return 0;
}
