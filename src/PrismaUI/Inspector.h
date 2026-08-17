#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

#include <memory>

namespace PrismaUI::Core {
typedef uint64_t PrismaViewId;
struct PrismaView;
}

namespace PrismaUI::Inspector {
using namespace ultralight;

void CreateInspectorView(const Core::PrismaViewId& viewId);
void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool visible);
bool IsInspectorVisible(const Core::PrismaViewId& viewId);
void SetInspectorBounds(const Core::PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width,
                        uint32_t height);
void RenderInspectorView(std::shared_ptr<Core::PrismaView> viewData);
void CopyInspectorBitmapToBuffer(std::shared_ptr<Core::PrismaView> viewData);
void CopyInspectorPixelsToTexture(Core::PrismaView* viewData, void* pixels, uint32_t width, uint32_t height,
                                  uint32_t stride);
void ReleaseInspectorTexture(Core::PrismaView* viewData);
void ClearInspectorBuffers(Core::PrismaView* viewData);
void DestroyInspectorResources(Core::PrismaView* viewData);
void EnsureInspectorAssetsAvailability();
bool AreInspectorAssetsAvailable();

}
