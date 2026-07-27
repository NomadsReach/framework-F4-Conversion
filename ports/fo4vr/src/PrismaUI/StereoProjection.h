#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace PrismaUI::StereoProjection
{
    enum class CaptureStage : std::uint8_t
    {
        None,
        RelocationResolved,
        RuntimeRootRead,
        RootStereoStateRead,
        StereoRecordsRead,
        Validated
    };

    struct Snapshot
    {
        DirectX::XMFLOAT4X4 composite[2]{};
        DirectX::XMFLOAT4 origin[2]{};
    };

    [[nodiscard]] bool CaptureSnapshot(
        Snapshot& outSnapshot) noexcept;
}
