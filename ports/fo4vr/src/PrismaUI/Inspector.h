#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/View.h>
#pragma warning(pop)

#include "PrismaUI/Core.h"

#include <cstdint>

namespace PrismaUI::Inspector
{
    void CreateInspectorView(Core::PrismaViewId viewId) noexcept;
    void SetInspectorVisibility(
        Core::PrismaViewId viewId,
        bool visible) noexcept;
    [[nodiscard]] bool IsInspectorVisible(
        Core::PrismaViewId viewId) noexcept;
    void SetInspectorBounds(
        Core::PrismaViewId viewId,
        float topLeftX,
        float topLeftY,
        std::uint32_t width,
        std::uint32_t height) noexcept;

    void EnsureAssetsAvailability() noexcept;
    [[nodiscard]] bool AreAssetsAvailable() noexcept;

    // Called synchronously by Ultralight on its worker while satisfying
    // ViewListener::OnCreateInspectorView.
    [[nodiscard]] ultralight::RefPtr<ultralight::View>
        HandleCreateRequest(Core::PrismaViewId viewId) noexcept;
}
