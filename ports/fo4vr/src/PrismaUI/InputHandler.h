#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/KeyEvent.h>
#include <Ultralight/MouseEvent.h>
#include <Ultralight/ScrollEvent.h>
#pragma warning(pop)

#include "PrismaUI/Core.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace PrismaUI::InputHandler
{
    struct LogicalViewportSize
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct CursorPosition
    {
        int x = 0;
        int y = 0;
    };

    enum class SpatialPointerEventKind : std::uint8_t
    {
        Move,
        Down,
        Up,
        Scroll
    };

    struct SpatialPointerEvent
    {
        Core::PrismaViewId viewId = 0;
        SpatialPointerEventKind kind =
            SpatialPointerEventKind::Move;
        int x = -1;
        int y = -1;
        int deltaX = 0;
        int deltaY = 0;
        bool forced = false;
        bool buttonDown = false;
    };

    inline constexpr std::size_t kInputEventCapacity = 4096;

    void Initialize(HWND gameWindow) noexcept;

    [[nodiscard]] bool InstallWindowHook() noexcept;
    void UninstallWindowHook() noexcept;

    [[nodiscard]] bool EnableInputCapture(
        Core::PrismaViewId viewId) noexcept;
    void DisableInputCapture(Core::PrismaViewId viewId) noexcept;
    void ClearImeState(Core::PrismaViewId viewId) noexcept;
    void RefreshTextInputTracking(Core::PrismaViewId viewId) noexcept;

    [[nodiscard]] bool IsInputCaptureActiveForView(
        Core::PrismaViewId viewId) noexcept;
    [[nodiscard]] bool IsAnyInputCaptureActive() noexcept;
    [[nodiscard]] Core::PrismaViewId GetFocusedViewId() noexcept;

    void SetLogicalViewportSize(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] LogicalViewportSize GetLogicalViewportSize() noexcept;
    [[nodiscard]] CursorPosition GetLastCursorPosition() noexcept;
    void RequestGameCursorSuppression() noexcept;

    [[nodiscard]] bool EnqueueSpatialPointerEvents(
        std::span<const SpatialPointerEvent> events) noexcept;
    [[nodiscard]] bool ScheduleSpatialPointerEventProcessing() noexcept;
    void FlushSpatialPointerEvents() noexcept;

    void ProcessEvents() noexcept;
    void Shutdown() noexcept;
}
