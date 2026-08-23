#pragma once

#include "Core.h"

namespace PrismaUI::NotificationSystem {
Core::PrismaViewId ShowNotification(const std::string& title, const std::string& message,
                                    uint32_t duration = 8000, const std::string& color = "warning");
void DismissNotification(Core::PrismaViewId notifId);
void ShowOverlayConflictWarning();
}
