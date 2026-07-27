#pragma once

#include "PrismaUI/SceneDepthCapture.h"

namespace PrismaUI::ViewRenderer
{
    enum class RenderLayout
    {
        Flat,
        SideBySideStereo,
        HeadLockedStereo
    };

    // Worker publication boundary after Renderer::Render.
    void PublishRenderTargets() noexcept;

    // Render-thread commit after the matching GPU command batch succeeds.
    void CommitPublishedRenderTargets() noexcept;

    // Render-thread operations. Core owns the complete context-state and
    // presentation lifecycle boundaries around these calls.
    void DrawViews(
        RenderLayout layout,
        const SceneDepthCapture::FrameDepth* sceneDepth) noexcept;
    void DrawCursor(RenderLayout layout) noexcept;
    void ReleaseDeviceResources() noexcept;
}
