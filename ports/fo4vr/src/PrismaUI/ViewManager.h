#pragma once

#include "PrismaUI/Core.h"

#include <functional>
#include <memory>
#include <string>

namespace PrismaUI::ViewManager
{
    [[nodiscard]] Core::PrismaViewId Create(
        const std::string& htmlPath,
        std::function<void(Core::PrismaViewId)> onDomReady = nullptr,
        PRISMA_UI_VR_API::NetworkAccessPolicy networkPolicy =
            PRISMA_UI_VR_API::NetworkAccessPolicy::Unrestricted);

    void Show(Core::PrismaViewId viewId) noexcept;
    void Hide(Core::PrismaViewId viewId) noexcept;
    [[nodiscard]] bool IsHidden(Core::PrismaViewId viewId) noexcept;
    [[nodiscard]] bool Focus(
        Core::PrismaViewId viewId,
        bool pauseGame = false,
        bool disableFocusMenu = false) noexcept;
    void Unfocus(Core::PrismaViewId viewId) noexcept;
    [[nodiscard]] bool HasFocus(Core::PrismaViewId viewId) noexcept;
    [[nodiscard]] bool ViewHasInputFocus(Core::PrismaViewId viewId) noexcept;
    void Destroy(Core::PrismaViewId viewId) noexcept;
    [[nodiscard]] bool IsValid(Core::PrismaViewId viewId) noexcept;

    void SetScrollingPixelSize(Core::PrismaViewId viewId, int pixelSize) noexcept;
    [[nodiscard]] int GetScrollingPixelSize(Core::PrismaViewId viewId) noexcept;
    void SetOrder(Core::PrismaViewId viewId, int order) noexcept;
    [[nodiscard]] int GetOrder(Core::PrismaViewId viewId) noexcept;

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

    [[nodiscard]] bool HasAnyActiveFocus() noexcept;
    void ReleaseAllFocus() noexcept;
    void CancelDeferredFocus(
        const std::shared_ptr<Core::PrismaView>& view) noexcept;
    void ApplyDeferredFocusIfReady(
        const std::shared_ptr<Core::PrismaView>& view) noexcept;

    void RegisterConsoleCallback(
        Core::PrismaViewId viewId,
        std::function<void(
            Core::PrismaViewId,
            PRISMA_UI_API::ConsoleMessageLevel,
            const std::string&)>
            callback) noexcept;
    void RegisterTranslations(
        Core::PrismaViewId viewId,
        const std::string& pluginName) noexcept;
    void EnumerateViews(
        const std::function<void(
            Core::PrismaViewId,
            const std::string&)>& callback) noexcept;

    [[nodiscard]] bool SetNetworkAccessPolicy(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::NetworkAccessPolicy policy) noexcept;
    [[nodiscard]] bool GetNetworkAccessPolicy(
        Core::PrismaViewId viewId,
        PRISMA_UI_VR_API::NetworkAccessPolicy& outPolicy) noexcept;
}
