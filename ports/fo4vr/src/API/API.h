#pragma once

#include "PrismaUI_F4_API.h"
#include "PrismaUI_F4VR_API.h"

namespace PluginAPI
{
    class BaseInterface final :
        public PRISMA_UI_API::IVPrismaUI4
    {
    public:
        [[nodiscard]] static BaseInterface& GetSingleton() noexcept;

        PrismaView CreateView(
            const char* htmlPath,
            PRISMA_UI_API::OnDomReadyCallback callback) noexcept override;
        void Invoke(
            PrismaView view,
            const char* script,
            PRISMA_UI_API::JSCallback callback) noexcept override;
        void InteropCall(
            PrismaView view,
            const char* functionName,
            const char* argument) noexcept override;
        void RegisterJSListener(
            PrismaView view,
            const char* functionName,
            PRISMA_UI_API::JSListenerCallback callback) noexcept override;
        bool HasFocus(PrismaView view) noexcept override;
        bool Focus(
            PrismaView view,
            bool pauseGame,
            bool disableFocusMenu) noexcept override;
        void Unfocus(PrismaView view) noexcept override;
        void Show(PrismaView view) noexcept override;
        void Hide(PrismaView view) noexcept override;
        bool IsHidden(PrismaView view) noexcept override;
        int GetScrollingPixelSize(PrismaView view) noexcept override;
        void SetScrollingPixelSize(
            PrismaView view,
            int pixelSize) noexcept override;
        bool IsValid(PrismaView view) noexcept override;
        void Destroy(PrismaView view) noexcept override;
        void SetOrder(
            PrismaView view,
            int order) noexcept override;
        int GetOrder(PrismaView view) noexcept override;
        void CreateInspectorView(
            PrismaView view) noexcept override;
        void SetInspectorVisibility(
            PrismaView view,
            bool visible) noexcept override;
        bool IsInspectorVisible(
            PrismaView view) noexcept override;
        void SetInspectorBounds(
            PrismaView view,
            float topLeftX,
            float topLeftY,
            unsigned int width,
            unsigned int height) noexcept override;
        bool HasAnyActiveFocus() noexcept override;
        void RegisterConsoleCallback(
            PrismaView view,
            PRISMA_UI_API::ConsoleMessageCallback callback) noexcept override;
        void RegisterTranslations(
            PrismaView view,
            const char* pluginName) noexcept override;
        void BindUIEvent(
            PrismaView view,
            const char* functionName,
            PRISMA_UI_API::JSListenerCallback callback) noexcept override;
        void EnumerateViews(
            PRISMA_UI_API::ViewEnumCallback callback,
            void* userData) noexcept override;

    private:
        BaseInterface() = default;
    };

    class VrInterface final :
        public PRISMA_UI_VR_API::IVPrismaUIVR1
    {
    public:
        [[nodiscard]] static VrInterface& GetSingleton() noexcept;

        PRISMA_UI_VR_API::SpatialResult GetSpatialCapabilities(
            PRISMA_UI_VR_API::SpatialCapabilitiesV1*
                outCapabilities) noexcept override;
        PRISMA_UI_VR_API::SpatialResult SubmitSpatialUpdate(
            PrismaView view,
            const PRISMA_UI_VR_API::SpatialUpdateV1*
                update) noexcept override;
        PRISMA_UI_VR_API::SpatialResult GetSpatialState(
            PrismaView view,
            PRISMA_UI_VR_API::SpatialStateV1*
                outState) noexcept override;
        PrismaView CreateViewWithOptions(
            const char* htmlPath,
            PRISMA_UI_API::OnDomReadyCallback callback,
            const PRISMA_UI_VR_API::ViewCreateOptionsV1*
                options) noexcept override;
        bool SetNetworkAccessPolicy(
            PrismaView view,
            PRISMA_UI_VR_API::NetworkAccessPolicy
                policy) noexcept override;
        bool GetNetworkAccessPolicy(
            PrismaView view,
            PRISMA_UI_VR_API::NetworkAccessPolicy*
                outPolicy) noexcept override;
        PRISMA_UI_VR_API::SpatialResult
            SubmitSpatialPointerUpdate(
                PrismaView view,
                const PRISMA_UI_VR_API::
                    SpatialPointerUpdateV1*
                    update) noexcept override;
        PRISMA_UI_VR_API::SpatialResult CancelSpatialPointer(
            PrismaView view) noexcept override;
        PRISMA_UI_VR_API::SpatialResult GetSpatialPointerState(
            PrismaView view,
            PRISMA_UI_VR_API::SpatialPointerStateV1*
                outState) noexcept override;

    private:
        VrInterface() = default;
    };
}
