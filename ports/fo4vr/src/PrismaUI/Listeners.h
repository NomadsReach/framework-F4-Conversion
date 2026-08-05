#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/String.h>
#include <Ultralight/NetworkRequest.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

#include "PrismaUI/Core.h"

#include <atomic>
#include <cstdint>

namespace PrismaUI::Listeners
{
    class LoadListener final : public ultralight::LoadListener
    {
    public:
        explicit LoadListener(Core::PrismaViewId viewId) noexcept;
        ~LoadListener() override = default;

        void OnBeginLoading(
            ultralight::View* caller,
            std::uint64_t frameId,
            bool isMainFrame,
            const ultralight::String& url) noexcept override;
        void OnFinishLoading(
            ultralight::View* caller,
            std::uint64_t frameId,
            bool isMainFrame,
            const ultralight::String& url) noexcept override;
        void OnFailLoading(
            ultralight::View* caller,
            std::uint64_t frameId,
            bool isMainFrame,
            const ultralight::String& url,
            const ultralight::String& description,
            const ultralight::String& errorDomain,
            int errorCode) noexcept override;
        void OnWindowObjectReady(
            ultralight::View* caller,
            std::uint64_t frameId,
            bool isMainFrame,
            const ultralight::String& url) noexcept override;
        void OnDOMReady(
            ultralight::View* caller,
            std::uint64_t frameId,
            bool isMainFrame,
            const ultralight::String& url) noexcept override;

    private:
        Core::PrismaViewId viewId_ = 0;
        std::atomic<std::uint32_t> lifecycleLogCount_ = 0;
    };

    class ViewListener final : public ultralight::ViewListener
    {
    public:
        explicit ViewListener(Core::PrismaViewId viewId) noexcept;
        ~ViewListener() override = default;

        void OnAddConsoleMessage(
            ultralight::View* caller,
            const ultralight::ConsoleMessage& message) noexcept override;
        ultralight::RefPtr<ultralight::View> OnCreateChildView(
            ultralight::View* caller,
            const ultralight::String& openerUrl,
            const ultralight::String& targetUrl,
            bool isPopup,
            const ultralight::IntRect& popupRect) noexcept override;
        ultralight::RefPtr<ultralight::View> OnCreateInspectorView(
            ultralight::View* caller,
            bool isLocal,
            const ultralight::String& inspectedUrl) noexcept override;

    private:
        Core::PrismaViewId viewId_ = 0;
        std::atomic<std::uint32_t> blockedChildViewCount_ = 0;
    };

    class NetworkListener final : public ultralight::NetworkListener
    {
    public:
        NetworkListener(
            Core::PrismaViewId viewId,
            const std::atomic<
                PRISMA_UI_VR_API::NetworkAccessPolicy>* policy) noexcept;
        ~NetworkListener() override = default;

        bool OnNetworkRequest(
            ultralight::View* caller,
            ultralight::NetworkRequest& request) noexcept override;

    private:
        Core::PrismaViewId viewId_ = 0;
        const std::atomic<
            PRISMA_UI_VR_API::NetworkAccessPolicy>* policy_ = nullptr;
        std::atomic<std::uint32_t> blockedRequestCount_ = 0;
    };

    class UltralightLogger final : public ultralight::Logger
    {
    public:
        ~UltralightLogger() override = default;
        void LogMessage(
            ultralight::LogLevel level,
            const ultralight::String& message) override;

    private:
        std::atomic<std::uint32_t> errorCount_ = 0;
    };
}
