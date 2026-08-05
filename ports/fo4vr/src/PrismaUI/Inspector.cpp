#include "PCH.h"

#include "PrismaUI/Inspector.h"

#include "PrismaUI/InputHandler.h"
#include "Utils/DllLoader.h"

namespace PrismaUI::Inspector
{
    namespace
    {
        std::once_flag g_assetCheck;
        std::atomic<bool> g_assetsAvailable = false;

        [[nodiscard]] Core::InspectorPresentationState
            DefaultPresentation() noexcept
        {
            const auto viewport =
                InputHandler::GetLogicalViewportSize();
            const auto width = (std::min)(
                viewport.width > 0 ? viewport.width : 1024u,
                1024u);
            const auto height = (std::min)(
                viewport.height > 0 ? viewport.height : 720u,
                720u);
            return {
                .x = 0.0f,
                .y = 0.0f,
                .width = (std::max)(width, 32u),
                .height = (std::max)(height, 32u),
                .opacity = 1.0f
            };
        }

        [[nodiscard]] bool Post(
            SingleThreadExecutor::Priority priority,
            std::function<void()> operation) noexcept
        {
            auto& worker = Core::GetRuntime().worker;
            if (worker.IsWorkerThread()) {
                try {
                    operation();
                    return true;
                } catch (...) {
                    return false;
                }
            }
            return worker.TryPost(priority, std::move(operation));
        }

        [[nodiscard]] bool SamePresentation(
            const Core::InspectorPresentationState& left,
            const Core::InspectorPresentationState& right) noexcept
        {
            return left.x == right.x &&
                   left.y == right.y &&
                   left.width == right.width &&
                   left.height == right.height &&
                   left.opacity == right.opacity;
        }
    }

    void EnsureAssetsAvailability() noexcept
    {
        std::call_once(g_assetCheck, [] {
            try {
                const auto framework = Utils::GetFrameworkPath();
                if (!framework) {
                    logger::warn(
                        "PrismaUI could not locate its inspector assets");
                    return;
                }
                std::error_code error;
                const auto main =
                    *framework / L"inspector" / L"Main.html";
                const auto available =
                    std::filesystem::is_regular_file(main, error) &&
                    !error;
                g_assetsAvailable.store(
                    available,
                    std::memory_order_release);
                if (!available) {
                    logger::warn(
                        "PrismaUI inspector assets are unavailable");
                }
            } catch (...) {
                logger::warn(
                    "PrismaUI failed to verify inspector assets");
            }
        });
    }

    bool AreAssetsAvailable() noexcept
    {
        EnsureAssetsAvailability();
        return g_assetsAvailable.load(std::memory_order_acquire);
    }

    void CreateInspectorView(
        Core::PrismaViewId viewId) noexcept
    {
        if (!AreAssetsAvailable()) {
            return;
        }
        const auto view = Core::FindView(viewId);
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return;
        }

        if (!Post(
                SingleThreadExecutor::Priority::MEDIUM,
                [view] {
                    if (view->destroying.load(
                            std::memory_order_acquire) ||
                        !view->ultralightView ||
                        view->inspectorView) {
                        return;
                    }
                    view->ultralightView->
                        CreateLocalInspectorView();
                })) {
            logger::warn(
                "View [{}] inspector creation was rejected",
                viewId);
        }
    }

    void SetInspectorVisibility(
        Core::PrismaViewId viewId,
        bool visible) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return;
        }
        if (visible && !AreAssetsAvailable()) {
            return;
        }

        if (!Post(
                SingleThreadExecutor::Priority::MEDIUM,
                [view, visible] {
                    if (view->destroying.load(
                            std::memory_order_acquire)) {
                        return;
                    }
                    if (visible &&
                        !view->inspectorView &&
                        view->ultralightView) {
                        view->ultralightView->
                            CreateLocalInspectorView();
                    }
                    if (!view->inspectorView) {
                        return;
                    }

                    view->inspectorVisible.store(
                        visible,
                        std::memory_order_release);
                    view->inspectorPointerHover.store(
                        false,
                        std::memory_order_release);
                    if (visible) {
                        view->inspectorView->Focus();
                        if (view->ultralightView) {
                            view->ultralightView->Unfocus();
                        }
                    } else {
                        view->inspectorView->Unfocus();
                        if (view->focused.load(
                                std::memory_order_acquire) &&
                            view->ultralightView) {
                            view->ultralightView->Focus();
                        }
                    }
                })) {
            logger::warn(
                "View [{}] inspector visibility change was rejected",
                viewId);
        }
    }

    bool IsInspectorVisible(
        Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        return view &&
               view->inspectorVisible.load(
                   std::memory_order_acquire);
    }

    void SetInspectorBounds(
        Core::PrismaViewId viewId,
        float topLeftX,
        float topLeftY,
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        if (!std::isfinite(topLeftX) ||
            !std::isfinite(topLeftY) ||
            width < 32 ||
            height < 32 ||
            width > Core::kMaximumAcceleratedDimension ||
            height > Core::kMaximumAcceleratedDimension) {
            logger::warn(
                "View [{}] rejected invalid inspector bounds",
                viewId);
            return;
        }

        const auto view = Core::FindView(viewId);
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return;
        }

        const auto viewport =
            InputHandler::GetLogicalViewportSize();
        const auto extentWidth =
            viewport.width > 0 ? viewport.width : width;
        const auto extentHeight =
            viewport.height > 0 ? viewport.height : height;
        Core::InspectorPresentationState requested{
            .x = std::clamp(
                topLeftX,
                0.0f,
                static_cast<float>(
                    extentWidth > width ?
                        extentWidth - width :
                        0u)),
            .y = std::clamp(
                topLeftY,
                0.0f,
                static_cast<float>(
                    extentHeight > height ?
                        extentHeight - height :
                        0u)),
            .width = width,
            .height = height,
            .opacity = 1.0f
        };

        Core::InspectorPresentationState previous;
        {
            std::lock_guard lock(
                view->inspectorPresentationMutex);
            previous = view->inspectorPresentation;
            view->inspectorPresentation = requested;
        }
        view->inspectorPointerHover.store(
            false,
            std::memory_order_release);

        const auto accepted = Post(
            SingleThreadExecutor::Priority::MEDIUM,
            [view, requested, previous] {
                if (view->destroying.load(
                        std::memory_order_acquire) ||
                    !view->inspectorView) {
                    return;
                }
                if (!Core::ResizeInspectorView(
                        view,
                        requested.width,
                        requested.height)) {
                    std::lock_guard lock(
                        view->inspectorPresentationMutex);
                    if (SamePresentation(
                            view->inspectorPresentation,
                            requested)) {
                        view->inspectorPresentation = previous;
                    }
                }
            });
        if (!accepted) {
            {
                std::lock_guard lock(
                    view->inspectorPresentationMutex);
                if (SamePresentation(
                        view->inspectorPresentation,
                        requested)) {
                    view->inspectorPresentation = previous;
                }
            }
            logger::warn(
                "View [{}] inspector resize was rejected",
                viewId);
        }
    }

    ultralight::RefPtr<ultralight::View> HandleCreateRequest(
        Core::PrismaViewId viewId) noexcept
    {
        auto& runtime = Core::GetRuntime();
        if (!runtime.worker.IsWorkerThread() ||
            !runtime.renderer ||
            !AreAssetsAvailable()) {
            return nullptr;
        }

        const auto view = Core::FindView(viewId);
        if (!view ||
            view->destroying.load(std::memory_order_acquire)) {
            return nullptr;
        }
        if (view->inspectorView) {
            return view->inspectorView;
        }

        Core::InspectorPresentationState presentation;
        {
            std::lock_guard lock(
                view->inspectorPresentationMutex);
            presentation = view->inspectorPresentation;
            if (presentation.width == 0 ||
                presentation.height == 0) {
                presentation = DefaultPresentation();
                view->inspectorPresentation = presentation;
            }
        }

        if (!Core::TryReserveInspectorPixels(
                view,
                presentation.width,
                presentation.height)) {
            logger::warn(
                "View [{}] inspector exceeds the accelerated pixel budget",
                viewId);
            return nullptr;
        }

        try {
            ultralight::ViewConfig config;
            config.is_accelerated = true;
            config.is_transparent = false;
            config.initial_focus = false;
            auto inspector = runtime.renderer->CreateView(
                presentation.width,
                presentation.height,
                config,
                nullptr);
            if (!inspector) {
                Core::ReleaseInspectorPixelReservation(view);
                return nullptr;
            }
            view->inspectorView = inspector;
            return inspector;
        } catch (...) {
            Core::ReleaseInspectorPixelReservation(view);
            logger::error(
                "View [{}] inspector creation failed",
                viewId);
            return nullptr;
        }
    }
}
