#pragma once

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace PrismaUI::Utils {

inline std::filesystem::path GetBasePath()
{
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(executable).parent_path() / L"Data" / L"PrismaUI_F4";
}

class DllLoader {
public:
    static DllLoader& GetSingleton()
    {
        static DllLoader instance;
        return instance;
    }

    bool LoadUltralightLibraries()
    {
        std::lock_guard lock(mutex_);
        if (loaded_.load(std::memory_order_acquire)) return true;

        const auto libsPath = GetBasePath() / L"libs";
        std::error_code error;
        if (libsPath.empty() || !std::filesystem::is_directory(libsPath, error) || error) {
            logger::error("Ultralight libs directory unavailable: {}", libsPath.string());
            return false;
        }

        static constexpr const wchar_t* names[] = {
            L"UltralightCore.dll",
            L"WebCore.dll",
            L"Ultralight.dll",
            L"AppCore.dll",
        };

        for (const wchar_t* name : names) {
            const auto path = libsPath / name;
            error.clear();
            if (!std::filesystem::is_regular_file(path, error) || error) {
                logger::error("Ultralight DLL unavailable: {}", path.string());
                UnloadAllInternal();
                return false;
            }

            HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) {
                logger::error("Failed to load {} (GLE={})", path.string(), GetLastError());
                UnloadAllInternal();
                return false;
            }
            modules_.push_back(module);
        }

        loaded_.store(true, std::memory_order_release);
        logger::info("Ultralight libraries loaded");
        return true;
    }

    void UnloadAll()
    {
        std::lock_guard lock(mutex_);
        UnloadAllInternal();
    }

    bool IsLoaded() const noexcept
    {
        return loaded_.load(std::memory_order_acquire);
    }

private:
    DllLoader() = default;
    ~DllLoader() = default;

    DllLoader(const DllLoader&) = delete;
    DllLoader& operator=(const DllLoader&) = delete;

    void UnloadAllInternal()
    {
        for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
            if (*it) FreeLibrary(*it);
        }
        modules_.clear();
        loaded_.store(false, std::memory_order_release);
    }

    std::vector<HMODULE> modules_;
    std::atomic<bool> loaded_{false};
    mutable std::mutex mutex_;
};

}
