#include "PCH.h"

#include "PrismaUI/StereoProjection.h"

#include <array>

namespace PrismaUI::StereoProjection
{
    namespace
    {
        constexpr std::uintptr_t kRuntimeRootRva = 0x6235AC8;
        constexpr std::uintptr_t kFirstOriginOffset = 0x2590;
        constexpr std::uintptr_t kSecondOriginOffset = 0x25A0;
        constexpr std::uintptr_t kRecordsPointerOffset = 0x25D0;
        constexpr std::uintptr_t kRecordStride = 0x210;
        constexpr std::uintptr_t kCompositeOffset = 0xD0;
        constexpr std::uintptr_t kFirstCompositeOffset =
            kCompositeOffset;
        constexpr std::uintptr_t kSecondCompositeOffset =
            kRecordStride + kCompositeOffset;
        constexpr std::size_t kRecordsReadBytes =
            kSecondCompositeOffset +
            sizeof(DirectX::XMFLOAT4X4);
        constexpr float kMaximumMagnitude = 1.0e8f;

        struct RootFields
        {
            float firstOrigin[4]{};
            float secondOrigin[4]{};
            std::byte unused[0x20]{};
            std::uintptr_t records = 0;
        };

        static_assert(
            offsetof(RootFields, secondOrigin) ==
            kSecondOriginOffset - kFirstOriginOffset);
        static_assert(
            offsetof(RootFields, records) ==
            kRecordsPointerOffset - kFirstOriginOffset);
        static_assert(sizeof(RootFields) == 0x48);

        std::atomic<CaptureStage> g_lastFailure =
            CaptureStage::None;
        std::atomic<bool> g_lastSucceeded = true;
        std::atomic<std::int64_t> g_lastLogMilliseconds = 0;

        [[nodiscard]] bool PlausiblePointer(
            std::uintptr_t value) noexcept
        {
            return value >= 0x10000u &&
                   value <= 0x00007FFFFFFFFFFFull &&
                   value % alignof(void*) == 0;
        }

        [[nodiscard]] bool Read(
            std::uintptr_t address,
            void* destination,
            std::size_t size,
            DWORD& error) noexcept
        {
            if (!PlausiblePointer(address) ||
                !destination ||
                size == 0 ||
                address >
                    (std::numeric_limits<std::uintptr_t>::max)() -
                        size) {
                error = ERROR_INVALID_ADDRESS;
                return false;
            }
            SIZE_T bytesRead = 0;
            if (!ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(address),
                    destination,
                    size,
                    &bytesRead) ||
                bytesRead != size) {
                error = GetLastError();
                if (error == ERROR_SUCCESS) {
                    error = ERROR_PARTIAL_COPY;
                }
                return false;
            }
            error = ERROR_SUCCESS;
            return true;
        }

        [[nodiscard]] bool ValidVector(
            const float* value) noexcept
        {
            return value &&
                   std::all_of(
                       value,
                       value + 3,
                       [](float component) {
                           return std::isfinite(component) &&
                                  std::fabs(component) <=
                                      kMaximumMagnitude;
                       });
        }

        [[nodiscard]] bool ValidMatrix(
            const DirectX::XMFLOAT4X4& matrix) noexcept
        {
            const auto values =
                reinterpret_cast<const float*>(&matrix);
            auto nonzero = false;
            for (std::size_t index = 0;
                 index < 16;
                 ++index) {
                if (!std::isfinite(values[index]) ||
                    std::fabs(values[index]) >
                        kMaximumMagnitude) {
                    return false;
                }
                nonzero =
                    nonzero ||
                    std::fabs(values[index]) > 1.0e-7f;
            }
            return nonzero;
        }

        [[nodiscard]] const char* StageName(
            CaptureStage stage) noexcept
        {
            switch (stage) {
            case CaptureStage::None:
                return "none";
            case CaptureStage::RelocationResolved:
                return "relocation";
            case CaptureStage::RuntimeRootRead:
                return "runtime-root";
            case CaptureStage::RootStereoStateRead:
                return "stereo-root";
            case CaptureStage::StereoRecordsRead:
                return "stereo-records";
            case CaptureStage::Validated:
                return "validated";
            }
            return "unknown";
        }

        void ReportFailure(
            CaptureStage stage,
            DWORD error) noexcept
        {
            const auto now =
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().
                        time_since_epoch())
                    .count();
            const auto previousStage =
                g_lastFailure.exchange(
                    stage,
                    std::memory_order_relaxed);
            const auto previousLog =
                g_lastLogMilliseconds.load(
                    std::memory_order_relaxed);
            g_lastSucceeded.store(
                false,
                std::memory_order_relaxed);
            if (previousStage != stage ||
                now - previousLog >= 5000) {
                g_lastLogMilliseconds.store(
                    now,
                    std::memory_order_relaxed);
                logger::warn(
                    "PrismaUI stereo projection unavailable at {} (Win32 error {})",
                    StageName(stage),
                    error);
            }
        }

        void ReportSuccess() noexcept
        {
            const auto wasSuccessful =
                g_lastSucceeded.exchange(
                    true,
                    std::memory_order_relaxed);
            g_lastFailure.store(
                CaptureStage::Validated,
                std::memory_order_relaxed);
            if (!wasSuccessful) {
                logger::info(
                    "PrismaUI stereo projection recovered");
            }
        }
    }

    bool CaptureSnapshot(Snapshot& outSnapshot) noexcept
    {
        outSnapshot = {};
        auto stage = CaptureStage::None;
        DWORD error = ERROR_SUCCESS;

        if (!REL::Module::IsVR() ||
            REL::Module::get().version() !=
                F4SE::RUNTIME_VR_1_2_72) {
            ReportFailure(stage, ERROR_NOT_SUPPORTED);
            return false;
        }

        try {
            const REL::Relocation<std::uintptr_t> rootAddress{
                REL::Offset(kRuntimeRootRva)
            };
            if (!PlausiblePointer(rootAddress.address())) {
                ReportFailure(stage, ERROR_INVALID_ADDRESS);
                return false;
            }
            stage = CaptureStage::RelocationResolved;

            std::uintptr_t root = 0;
            if (!Read(
                    rootAddress.address(),
                    &root,
                    sizeof(root),
                    error) ||
                !PlausiblePointer(root)) {
                ReportFailure(
                    stage,
                    error == ERROR_SUCCESS ?
                        ERROR_INVALID_ADDRESS :
                        error);
                return false;
            }
            stage = CaptureStage::RuntimeRootRead;

            RootFields fields;
            if (!Read(
                    root + kFirstOriginOffset,
                    &fields,
                    sizeof(fields),
                    error)) {
                ReportFailure(stage, error);
                return false;
            }
            stage = CaptureStage::RootStereoStateRead;
            if (!PlausiblePointer(fields.records)) {
                ReportFailure(stage, ERROR_INVALID_ADDRESS);
                return false;
            }

            std::array<std::byte, kRecordsReadBytes> records{};
            if (!Read(
                    fields.records,
                    records.data(),
                    records.size(),
                    error)) {
                ReportFailure(stage, error);
                return false;
            }
            stage = CaptureStage::StereoRecordsRead;

            std::memcpy(
                &outSnapshot.composite[0],
                records.data() + kFirstCompositeOffset,
                sizeof(outSnapshot.composite[0]));
            std::memcpy(
                &outSnapshot.composite[1],
                records.data() + kSecondCompositeOffset,
                sizeof(outSnapshot.composite[1]));
            outSnapshot.origin[0] = {
                fields.firstOrigin[0],
                fields.firstOrigin[1],
                fields.firstOrigin[2],
                0.0f
            };
            outSnapshot.origin[1] = {
                fields.secondOrigin[0],
                fields.secondOrigin[1],
                fields.secondOrigin[2],
                0.0f
            };
            if (!ValidVector(fields.firstOrigin) ||
                !ValidVector(fields.secondOrigin) ||
                !ValidMatrix(outSnapshot.composite[0]) ||
                !ValidMatrix(outSnapshot.composite[1])) {
                outSnapshot = {};
                ReportFailure(stage, ERROR_INVALID_DATA);
                return false;
            }

            ReportSuccess();
            return true;
        } catch (...) {
            outSnapshot = {};
            ReportFailure(stage, ERROR_UNHANDLED_EXCEPTION);
            return false;
        }
    }
}
