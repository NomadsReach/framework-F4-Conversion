#pragma once

#include <Windows.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace PrismaUI::Utils
{
    [[nodiscard]] std::optional<std::filesystem::path> GetFrameworkPath() noexcept;

    class DllLoader final
    {
    public:
        [[nodiscard]] static DllLoader& GetSingleton() noexcept;

        [[nodiscard]] bool LoadUltralightLibraries() noexcept;
        [[nodiscard]] bool IsLoaded() const noexcept;

        DllLoader(const DllLoader&) = delete;
        DllLoader& operator=(const DllLoader&) = delete;

    private:
        DllLoader() = default;
        ~DllLoader() = default;

        void UnloadPartialLoad() noexcept;

        mutable std::mutex mutex_;
        std::vector<HMODULE> modules_;
        bool loaded_ = false;
    };
}
