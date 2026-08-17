#pragma once

#include "PrismaUI_F4_API.h"

namespace PrismaUI::ConflictChecker {
    void CheckEarly();
    void CheckPreHooks();
    void OnAPIRequest(void* returnAddress, PRISMA_UI_API::InterfaceVersion version);
}
