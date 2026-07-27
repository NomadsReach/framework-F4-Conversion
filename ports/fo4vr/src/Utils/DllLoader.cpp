#include "PCH.h"

#include "Utils/DllLoader.h"

namespace PrismaUI::Utils
{
    std::optional<std::filesystem::path> GetFrameworkPath() noexcept
    {
        try {
            std::wstring modulePath(32768, L'\0');
            const auto length = GetModuleFileNameW(
                GetModuleHandleW(L"PrismaUI_F4.dll"),
                modulePath.data(),
                static_cast<DWORD>(modulePath.size()));
            if (length == 0 || length >= modulePath.size()) {
                return std::nullopt;
            }
            modulePath.resize(length);

            auto dataPath = std::filesystem::path(modulePath).parent_path();
            if (dataPath.filename() != L"Plugins" &&
                dataPath.filename() != L"plugins") {
                return std::nullopt;
            }
            dataPath = dataPath.parent_path();
            if (dataPath.filename() != L"F4SE" &&
                dataPath.filename() != L"f4se") {
                return std::nullopt;
            }
            dataPath = dataPath.parent_path();
            return dataPath / L"PrismaUI_F4";
        } catch (...) {
            return std::nullopt;
        }
    }

    DllLoader& DllLoader::GetSingleton() noexcept
    {
        static DllLoader loader;
        return loader;
    }

    bool DllLoader::LoadUltralightLibraries() noexcept
    {
        std::lock_guard lock(mutex_);
        if (loaded_) {
            return true;
        }

        const auto frameworkPath = GetFrameworkPath();
        if (!frameworkPath) {
            logger::error(
                "Could not derive Data/PrismaUI_F4 from the PrismaUI DLL path");
            return false;
        }

        const auto libraryPath = *frameworkPath / L"libs";
        constexpr std::array names{
            L"UltralightCore.dll",
            L"WebCore.dll",
            L"Ultralight.dll",
            L"AppCore.dll"
        };

        for (const auto* name : names) {
            const auto path = libraryPath / name;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error) {
                logger::error(
                    "Required Ultralight library is missing: {}",
                    path.filename().string());
                UnloadPartialLoad();
                return false;
            }

            const auto module = LoadLibraryExW(
                path.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) {
                logger::error(
                    "Failed to load {} (Win32 error {})",
                    path.filename().string(),
                    GetLastError());
                UnloadPartialLoad();
                return false;
            }
            modules_.push_back(module);
        }

        loaded_ = true;
        logger::info("Loaded the Ultralight runtime from Data/PrismaUI_F4/libs");
        return true;
    }

    bool DllLoader::IsLoaded() const noexcept
    {
        std::lock_guard lock(mutex_);
        return loaded_;
    }

    void DllLoader::UnloadPartialLoad() noexcept
    {
        for (auto iterator = modules_.rbegin(); iterator != modules_.rend(); ++iterator) {
            if (*iterator) {
                FreeLibrary(*iterator);
            }
        }
        modules_.clear();
        loaded_ = false;
    }
}
