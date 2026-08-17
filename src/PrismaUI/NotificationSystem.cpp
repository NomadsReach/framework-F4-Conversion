#include "NotificationSystem.h"

#include "../API/API.h"
#include "../PCH.h"
#include "Communication.h"
#include "ViewManager.h"

namespace PrismaUI::NotificationSystem {
    static std::atomic<Core::PrismaViewId> g_notificationViewId = 0;
    static std::atomic<bool> g_notificationInitialized = false;

    static auto EscapeJS(const std::string& s) -> std::string {
        std::string escaped;
        for (char c : s) {
            if (c == '\'') {
                escaped += "\\'";
            } else if (c == '\\') {
                escaped += "\\\\";
            } else if (c == '\n') {
                escaped += "\\n";
            } else {
                escaped += c;
            }
        }
        return escaped;
    }

    Core::PrismaViewId ShowNotification(const std::string& title, const std::string& message, uint32_t duration,
                                        const std::string& color) {
        logger::info("[NotificationSystem::ShowNotification] Attempting to show: '{}' | '{}'", title, message);

        if (!g_notificationInitialized) {
            logger::info("[NotificationSystem] Initializing notification view...");

            auto api = PluginAPI::PrismaUIInterface::GetSingleton();
            if (!api) {
                logger::critical("[NotificationSystem] PrismaUI API not available - this is a critical error!");
                logger::critical("[NotificationSystem] Make sure PrismaUI_F4.dll is loaded and initialized");
                return 0;
            }

            logger::info("[NotificationSystem] API available, creating view...");

            try {
                Core::PrismaViewId notifId = ViewManager::Create("notification-banner.html", [](Core::PrismaViewId viewId) {
                    logger::info("[NotificationSystem] Notification view DOM ready, ID: {}", viewId);
                    g_notificationViewId = viewId;
                    g_notificationInitialized = true;
                });

                if (notifId == 0) {
                    logger::error("[NotificationSystem] ViewManager::Create returned 0");
                    return 0;
                }

                logger::info("[NotificationSystem] View created with ID: {}, showing...", notifId);
                api->Show(notifId);
                logger::info("[NotificationSystem] Notification view shown");
            } catch (const std::exception& e) {
                logger::error("[NotificationSystem] Exception during initialization: {}", e.what());
                return 0;
            }
        }

        logger::info("[NotificationSystem] Attempting to invoke JS, notifId={}, initialized={}",
                     g_notificationViewId.load(), g_notificationInitialized.load());

        auto api = PluginAPI::PrismaUIInterface::GetSingleton();
        if (!api) {
            logger::error("[NotificationSystem] API became unavailable");
            return 0;
        }

        if (g_notificationViewId == 0) {
            logger::error("[NotificationSystem] View ID is still 0");
            return 0;
        }

        if (!api->IsValid(g_notificationViewId)) {
            logger::error("[NotificationSystem] View ID {} is not valid", g_notificationViewId.load());
            return 0;
        }

        char jsBuffer[1024];
        snprintf(jsBuffer, sizeof(jsBuffer), "window.showNotification('%s', '%s', %u, '%s')", EscapeJS(title).c_str(),
                 EscapeJS(message).c_str(), duration, color.c_str());

        logger::info("[NotificationSystem] Invoking JS: {}", jsBuffer);
        api->Invoke(g_notificationViewId, jsBuffer);
        logger::info("[NotificationSystem] JS invoked successfully");
        return g_notificationViewId;
    }

    void DismissNotification(Core::PrismaViewId notifId) {
        auto api = PluginAPI::PrismaUIInterface::GetSingleton();
        if (api && api->IsValid(notifId)) {
            api->Invoke(notifId, "window.dismissNotification && window.dismissNotification()");
        }
    }

    void ShowOverlayConflictWarning() {
        ShowNotification("GPU Overlay Detected",
                         "GPU monitoring software (RivaTuner, MSI Afterburner, etc.) may conflict with the UI.\n"
                         "If you experience issues, try disabling it.",
                         12000, "warning");
    }
}
