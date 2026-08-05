#include "PCH.h"

#include "API/API.h"

#include "PrismaUI/Communication.h"
#include "PrismaUI/SpatialPointer.h"
#include "PrismaUI/SpatialPresentation.h"
#include "PrismaUI/ViewManager.h"
#include "Utils/Encoding.h"

namespace
{
    [[nodiscard]] bool QueueGameTask(
        std::function<void()> operation) noexcept
    {
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks || !operation) {
            return false;
        }
        try {
            tasks->AddTask(
                [operation = std::move(operation)]() mutable noexcept {
                    try {
                        operation();
                    } catch (...) {
                        try {
                            logger::error(
                                "PrismaUI consumer callback raised an exception");
                        } catch (...) {
                        }
                    }
                });
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::function<void(
        PrismaUI::Core::PrismaViewId)>
        MakeDomReadyCallback(
            PRISMA_UI_API::OnDomReadyCallback callback)
    {
        if (!callback) {
            return {};
        }
        return [callback](
                   PrismaUI::Core::PrismaViewId view) {
            if (!QueueGameTask(
                    [callback, view] {
                        if (PrismaUI::ViewManager::
                                IsValid(view)) {
                            callback(
                                static_cast<PrismaView>(
                                    view));
                        }
                    })) {
                logger::error(
                    "View [{}] could not dispatch its DOM-ready callback",
                    view);
            }
        };
    }

    [[nodiscard]] std::optional<std::string> NormalizeText(
        const char* value)
    {
        if (!value) {
            return std::nullopt;
        }
        if (PrismaUI::Encoding::IsValidUtf8(value)) {
            return std::string(value);
        }
        auto converted =
            PrismaUI::Encoding::AnsiToUtf8(value);
        if (converted.empty() && value[0] != '\0') {
            return std::nullopt;
        }
        return converted;
    }

    [[nodiscard]] bool ValidViewOptions(
        const PRISMA_UI_VR_API::ViewCreateOptionsV1*
            options) noexcept
    {
        if (!options ||
            options->structSize < sizeof(*options)) {
            return false;
        }
        return std::all_of(
            std::begin(options->reserved),
            std::end(options->reserved),
            [](std::uint32_t value) {
                return value == 0;
            });
    }
}

namespace PluginAPI
{
    BaseInterface& BaseInterface::GetSingleton() noexcept
    {
        static BaseInterface singleton;
        return singleton;
    }

    PrismaView BaseInterface::CreateView(
        const char* htmlPath,
        PRISMA_UI_API::OnDomReadyCallback callback) noexcept
    {
        if (!htmlPath) {
            return 0;
        }
        try {
            return PrismaUI::ViewManager::Create(
                htmlPath,
                MakeDomReadyCallback(callback),
                PRISMA_UI_VR_API::
                    NetworkAccessPolicy::Unrestricted);
        } catch (...) {
            return 0;
        }
    }

    void BaseInterface::Invoke(
        PrismaView view,
        const char* script,
        PRISMA_UI_API::JSCallback callback) noexcept
    {
        if (view == 0 || !script) {
            return;
        }
        try {
            const auto normalized = NormalizeText(script);
            if (!normalized) {
                return;
            }
            std::function<void(std::string)> wrapped;
            if (callback) {
                wrapped = [callback, view](std::string result) {
                    if (!QueueGameTask(
                            [callback,
                             view,
                             result = std::move(result)] {
                                if (PrismaUI::ViewManager::
                                        IsValid(view)) {
                                    callback(result.c_str());
                                }
                            })) {
                        logger::error(
                            "PrismaUI could not dispatch a script callback");
                    }
                };
            }
            PrismaUI::Communication::Invoke(
                view,
                ultralight::String(
                    normalized->c_str()),
                std::move(wrapped));
        } catch (...) {
        }
    }

    void BaseInterface::InteropCall(
        PrismaView view,
        const char* functionName,
        const char* argument) noexcept
    {
        if (view == 0 ||
            !functionName ||
            !argument) {
            return;
        }
        try {
            const auto normalized =
                NormalizeText(argument);
            if (!normalized) {
                return;
            }
            PrismaUI::Communication::InteropCall(
                view,
                functionName,
                *normalized);
        } catch (...) {
        }
    }

    void BaseInterface::RegisterJSListener(
        PrismaView view,
        const char* functionName,
        PRISMA_UI_API::JSListenerCallback callback) noexcept
    {
        if (view == 0 ||
            !functionName ||
            !callback) {
            return;
        }
        try {
            PrismaUI::Communication::RegisterJSListener(
                view,
                functionName,
                [callback, view](std::string argument) {
                    if (!QueueGameTask(
                            [callback,
                             view,
                             argument = std::move(argument)] {
                                if (PrismaUI::ViewManager::
                                        IsValid(view)) {
                                    callback(argument.c_str());
                                }
                            })) {
                        logger::error(
                            "PrismaUI could not dispatch a JavaScript listener");
                    }
                });
        } catch (...) {
        }
    }

    bool BaseInterface::HasFocus(
        PrismaView view) noexcept
    {
        return view != 0 &&
               PrismaUI::ViewManager::HasFocus(view);
    }

    bool BaseInterface::Focus(
        PrismaView view,
        bool pauseGame,
        bool disableFocusMenu) noexcept
    {
        return view != 0 &&
               PrismaUI::ViewManager::Focus(
                   view,
                   pauseGame,
                   disableFocusMenu);
    }

    void BaseInterface::Unfocus(
        PrismaView view) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::Unfocus(view);
        }
    }

    void BaseInterface::Show(PrismaView view) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::Show(view);
        }
    }

    void BaseInterface::Hide(PrismaView view) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::Hide(view);
        }
    }

    bool BaseInterface::IsHidden(
        PrismaView view) noexcept
    {
        return view == 0 ||
               PrismaUI::ViewManager::IsHidden(view);
    }

    int BaseInterface::GetScrollingPixelSize(
        PrismaView view) noexcept
    {
        return view != 0 ?
            PrismaUI::ViewManager::
                GetScrollingPixelSize(view) :
            0;
    }

    void BaseInterface::SetScrollingPixelSize(
        PrismaView view,
        int pixelSize) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::
                SetScrollingPixelSize(
                    view,
                    pixelSize);
        }
    }

    bool BaseInterface::IsValid(
        PrismaView view) noexcept
    {
        return view != 0 &&
               PrismaUI::ViewManager::IsValid(view);
    }

    void BaseInterface::Destroy(
        PrismaView view) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::Destroy(view);
        }
    }

    void BaseInterface::SetOrder(
        PrismaView view,
        int order) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::SetOrder(
                view,
                order);
        }
    }

    int BaseInterface::GetOrder(
        PrismaView view) noexcept
    {
        return view != 0 ?
            PrismaUI::ViewManager::GetOrder(view) :
            -1;
    }

    void BaseInterface::CreateInspectorView(
        PrismaView view) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::
                CreateInspectorView(view);
        }
    }

    void BaseInterface::SetInspectorVisibility(
        PrismaView view,
        bool visible) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::
                SetInspectorVisibility(
                    view,
                    visible);
        }
    }

    bool BaseInterface::IsInspectorVisible(
        PrismaView view) noexcept
    {
        return view != 0 &&
               PrismaUI::ViewManager::
                   IsInspectorVisible(view);
    }

    void BaseInterface::SetInspectorBounds(
        PrismaView view,
        float topLeftX,
        float topLeftY,
        unsigned int width,
        unsigned int height) noexcept
    {
        if (view != 0) {
            PrismaUI::ViewManager::
                SetInspectorBounds(
                    view,
                    topLeftX,
                    topLeftY,
                    width,
                    height);
        }
    }

    bool BaseInterface::HasAnyActiveFocus() noexcept
    {
        return PrismaUI::ViewManager::
            HasAnyActiveFocus();
    }

    void BaseInterface::RegisterConsoleCallback(
        PrismaView view,
        PRISMA_UI_API::ConsoleMessageCallback callback) noexcept
    {
        if (view == 0) {
            return;
        }
        try {
            if (!callback) {
                PrismaUI::ViewManager::
                    RegisterConsoleCallback(
                        view,
                        nullptr);
                return;
            }
            PrismaUI::ViewManager::RegisterConsoleCallback(
                view,
                [callback](
                    PrismaUI::Core::PrismaViewId source,
                    PRISMA_UI_API::ConsoleMessageLevel level,
                    const std::string& message) {
                    if (!QueueGameTask(
                            [callback,
                             source,
                             level,
                             message] {
                                if (PrismaUI::ViewManager::
                                        IsValid(source)) {
                                    callback(
                                        source,
                                        level,
                                        message.c_str());
                                }
                            })) {
                        logger::error(
                            "View [{}] could not dispatch a console callback",
                            source);
                    }
                });
        } catch (...) {
        }
    }

    void BaseInterface::RegisterTranslations(
        PrismaView view,
        const char* pluginName) noexcept
    {
        if (view == 0 ||
            !pluginName ||
            pluginName[0] == '\0') {
            return;
        }
        try {
            PrismaUI::ViewManager::RegisterTranslations(
                view,
                pluginName);
        } catch (...) {
        }
    }

    void BaseInterface::BindUIEvent(
        PrismaView view,
        const char* functionName,
        PRISMA_UI_API::JSListenerCallback callback) noexcept
    {
        RegisterJSListener(
            view,
            functionName,
            callback);
    }

    void BaseInterface::EnumerateViews(
        PRISMA_UI_API::ViewEnumCallback callback,
        void* userData) noexcept
    {
        if (!callback) {
            return;
        }
        PrismaUI::ViewManager::EnumerateViews(
            [callback, userData](
                PrismaUI::Core::PrismaViewId view,
                const std::string& path) {
                try {
                    callback(
                        view,
                        path.c_str(),
                        userData);
                } catch (...) {
                    logger::error(
                        "PrismaUI view enumeration callback failed");
                }
            });
    }

    VrInterface& VrInterface::GetSingleton() noexcept
    {
        static VrInterface singleton;
        return singleton;
    }

    PRISMA_UI_VR_API::SpatialResult
        VrInterface::GetSpatialCapabilities(
            PRISMA_UI_VR_API::SpatialCapabilitiesV1*
                outCapabilities) noexcept
    {
        return PrismaUI::SpatialPresentation::
            GetCapabilities(outCapabilities);
    }

    PRISMA_UI_VR_API::SpatialResult
        VrInterface::SubmitSpatialUpdate(
            PrismaView view,
            const PRISMA_UI_VR_API::SpatialUpdateV1*
                update) noexcept
    {
        return PrismaUI::SpatialPresentation::
            SubmitUpdate(view, update);
    }

    PRISMA_UI_VR_API::SpatialResult
        VrInterface::GetSpatialState(
            PrismaView view,
            PRISMA_UI_VR_API::SpatialStateV1*
                outState) noexcept
    {
        return PrismaUI::SpatialPresentation::
            GetState(view, outState);
    }

    PrismaView VrInterface::CreateViewWithOptions(
        const char* htmlPath,
        PRISMA_UI_API::OnDomReadyCallback callback,
        const PRISMA_UI_VR_API::ViewCreateOptionsV1*
            options) noexcept
    {
        if (!htmlPath || !ValidViewOptions(options)) {
            return 0;
        }
        try {
            return PrismaUI::ViewManager::Create(
                htmlPath,
                MakeDomReadyCallback(callback),
                options->networkAccessPolicy);
        } catch (...) {
            return 0;
        }
    }

    bool VrInterface::SetNetworkAccessPolicy(
        PrismaView view,
        PRISMA_UI_VR_API::NetworkAccessPolicy
            policy) noexcept
    {
        return view != 0 &&
               PrismaUI::ViewManager::
                   SetNetworkAccessPolicy(
                       view,
                       policy);
    }

    bool VrInterface::GetNetworkAccessPolicy(
        PrismaView view,
        PRISMA_UI_VR_API::NetworkAccessPolicy*
            outPolicy) noexcept
    {
        if (view == 0 || !outPolicy) {
            return false;
        }
        return PrismaUI::ViewManager::
            GetNetworkAccessPolicy(
                view,
                *outPolicy);
    }

    PRISMA_UI_VR_API::SpatialResult
        VrInterface::SubmitSpatialPointerUpdate(
            PrismaView view,
            const PRISMA_UI_VR_API::
                SpatialPointerUpdateV1*
                update) noexcept
    {
        return PrismaUI::SpatialPointer::
            SubmitUpdate(view, update);
    }

    PRISMA_UI_VR_API::SpatialResult
        VrInterface::CancelSpatialPointer(
            PrismaView view) noexcept
    {
        return PrismaUI::SpatialPointer::Cancel(view);
    }

    PRISMA_UI_VR_API::SpatialResult
        VrInterface::GetSpatialPointerState(
            PrismaView view,
            PRISMA_UI_VR_API::SpatialPointerStateV1*
                outState) noexcept
    {
        return PrismaUI::SpatialPointer::
            GetState(view, outState);
    }
}

extern "C" PRISMA_DLLEXPORT void* F4SEAPI RequestPluginAPI(
    PRISMA_UI_API::InterfaceVersion version) noexcept
{
    auto& api = PluginAPI::BaseInterface::GetSingleton();
    switch (version) {
    case PRISMA_UI_API::InterfaceVersion::V1:
        return static_cast<
            PRISMA_UI_API::IVPrismaUI1*>(&api);
    case PRISMA_UI_API::InterfaceVersion::V2:
        return static_cast<
            PRISMA_UI_API::IVPrismaUI2*>(&api);
    case PRISMA_UI_API::InterfaceVersion::V3:
        return static_cast<
            PRISMA_UI_API::IVPrismaUI3*>(&api);
    case PRISMA_UI_API::InterfaceVersion::V4:
        return static_cast<
            PRISMA_UI_API::IVPrismaUI4*>(&api);
    default:
        return nullptr;
    }
}

extern "C" PRISMA_DLLEXPORT void* F4SEAPI RequestPluginVRAPI(
    PRISMA_UI_VR_API::InterfaceVersion version) noexcept
{
    if (version !=
        PRISMA_UI_VR_API::InterfaceVersion::V1) {
        return nullptr;
    }
    return static_cast<
        PRISMA_UI_VR_API::IVPrismaUIVR1*>(
        &PluginAPI::VrInterface::GetSingleton());
}
