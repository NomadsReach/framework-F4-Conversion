#include "RenderRetirement.h"

#include <mutex>
#include <vector>

#include "Core.h"
#include "Inspector.h"
#include "ViewRenderer.h"

namespace PrismaUI::RenderRetirement {
namespace {
std::mutex g_retirementMutex;
std::vector<std::shared_ptr<Core::PrismaView>> g_pendingRetirements;
}

void Enqueue(std::shared_ptr<Core::PrismaView> viewData)
{
    if (!viewData) return;
    std::lock_guard lock(g_retirementMutex);
    g_pendingRetirements.push_back(std::move(viewData));
}

void Drain()
{
    std::vector<std::shared_ptr<Core::PrismaView>> retirements;
    {
        std::lock_guard lock(g_retirementMutex);
        retirements.swap(g_pendingRetirements);
    }

    for (const auto& viewData : retirements) {
        if (!viewData) continue;
        ViewRenderer::ReleaseViewTexture(viewData.get());
        Inspector::ReleaseInspectorTexture(viewData.get());
        viewData->pendingResourceRelease.store(false, std::memory_order_release);
    }
}

void Abandon()
{
    std::lock_guard lock(g_retirementMutex);
    g_pendingRetirements.clear();
}

}
