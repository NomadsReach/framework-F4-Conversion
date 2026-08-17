#include "NotificationSystem.h"

#include <deque>
#include <mutex>
#include <vector>

#include "Communication.h"
#include "ViewManager.h"

namespace PrismaUI::NotificationSystem {
namespace {

struct Notification {
    std::string title;
    std::string message;
    uint32_t duration = 0;
    std::string color;
};

constexpr size_t kMaxPending = 64;
std::mutex g_mutex;
Core::PrismaViewId g_viewId = 0;
bool g_creating = false;
bool g_ready = false;
std::deque<Notification> g_pending;

std::string EscapeJs(const std::string& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() + 8);

    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (i + 2 < value.size() && c == 0xE2 && static_cast<unsigned char>(value[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(value[i + 2]) == 0xA8 || static_cast<unsigned char>(value[i + 2]) == 0xA9)) {
            output += static_cast<unsigned char>(value[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
            continue;
        }

        switch (c) {
            case '\'': output += "\\'"; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            default:
                if (c < 0x20) {
                    output += "\\x";
                    output.push_back(hex[(c >> 4) & 0xF]);
                    output.push_back(hex[c & 0xF]);
                } else {
                    output.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return output;
}

void InvokeNotification(Core::PrismaViewId viewId, const Notification& notification)
{
    if (!ViewManager::IsValid(viewId)) return;

    std::string script = "window.showNotification&&window.showNotification('";
    script += EscapeJs(notification.title);
    script += "','";
    script += EscapeJs(notification.message);
    script += "',";
    script += std::to_string(notification.duration);
    script += ",'";
    script += EscapeJs(notification.color);
    script += "')";
    Communication::Invoke(viewId, ultralight::String(script.c_str()), nullptr);
}

void OnReady(Core::PrismaViewId viewId)
{
    std::vector<Notification> pending;
    {
        std::lock_guard lock(g_mutex);
        if (g_viewId != viewId || !ViewManager::IsValid(viewId)) return;
        g_ready = true;
        g_creating = false;
        pending.assign(std::make_move_iterator(g_pending.begin()), std::make_move_iterator(g_pending.end()));
        g_pending.clear();
    }

    for (const auto& notification : pending) InvokeNotification(viewId, notification);
}

}

Core::PrismaViewId ShowNotification(const std::string& title, const std::string& message, uint32_t duration,
                                    const std::string& color)
{
    Notification notification{title, message, duration, color};
    Core::PrismaViewId currentView = 0;
    bool ready = false;
    bool create = false;

    {
        std::lock_guard lock(g_mutex);
        if (g_viewId != 0 && !ViewManager::IsValid(g_viewId)) {
            g_viewId = 0;
            g_ready = false;
            g_creating = false;
        }

        currentView = g_viewId;
        ready = g_ready && currentView != 0;
        if (!ready) {
            if (g_pending.size() >= kMaxPending) g_pending.pop_front();
            g_pending.push_back(notification);
            if (!g_creating) {
                g_creating = true;
                create = true;
            }
        }
    }

    if (ready) {
        InvokeNotification(currentView, notification);
        return currentView;
    }

    if (!create) return currentView;

    Core::PrismaViewId created = 0;
    try {
        created = ViewManager::Create("notification-banner.html", OnReady);
    } catch (const std::exception& e) {
        logger::error("Notification view creation failed: {}", e.what());
    } catch (...) {
        logger::error("Notification view creation failed");
    }

    {
        std::lock_guard lock(g_mutex);
        if (created == 0) {
            g_creating = false;
            return 0;
        }
        g_viewId = created;
        g_ready = false;
    }

    return created;
}

void DismissNotification(Core::PrismaViewId notifId)
{
    if (!notifId || !ViewManager::IsValid(notifId)) return;
    Communication::Invoke(notifId,
        ultralight::String("window.dismissNotification&&window.dismissNotification()"), nullptr);
}

void ShowOverlayConflictWarning()
{
    ShowNotification("GPU Overlay Detected",
                     "GPU monitoring software may conflict with the UI. If you experience issues, try disabling it.",
                     12000, "warning");
}

}
