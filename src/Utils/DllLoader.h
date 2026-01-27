#pragma once

#include <Windows.h>
#include <filesystem>
#include <vector>
#include <string>
#include <mutex>

// Note: This header depends on 'logger' namespace from PCH.h (SKSE::log)

namespace PrismaUI::Utils
{
    // Common base path for PrismaUI data files
    inline std::filesystem::path GetBasePath()
    {
        return std::filesystem::current_path() / "Data" / "PrismaUI";
    }

    class DllLoader
    {
    public:
        static DllLoader& GetSingleton()
        {
            static DllLoader instance;
            return instance;
        }

        // Load Ultralight DLLs from the specified path
        // Must be called before any Ultralight API usage
        bool LoadUltralightLibraries()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            
            if (m_loaded) {
                return true;
            }

            auto libsPath = GetBasePath() / "libs";

            if (!std::filesystem::exists(libsPath)) {
                logger::error("Ultralight libs path does not exist: {}", libsPath.string());
                return false;
            }

            // Load order matters due to dependencies:
            // UltralightCore -> WebCore -> Ultralight -> AppCore
            const std::vector<std::wstring> dllNames = {
                L"UltralightCore.dll",
                L"WebCore.dll",
                L"Ultralight.dll",
                L"AppCore.dll"
            };

            for (const auto& dllName : dllNames) {
                auto dllPath = libsPath / dllName;
                
                if (!std::filesystem::exists(dllPath)) {
                    logger::error("DLL not found: {}", dllPath.string());
                    UnloadAllInternal();
                    return false;
                }

                HMODULE handle = LoadLibraryW(dllPath.c_str());
                
                if (!handle) {
                    DWORD error = GetLastError();
                    logger::error("Failed to load DLL: {} (Error: {})", dllPath.string(), error);
                    UnloadAllInternal();
                    return false;
                }

                m_loadedModules.push_back(handle);
                logger::info("Loaded Ultralight DLL: {}", dllPath.filename().string());
            }

            m_loaded = true;
            logger::info("All Ultralight DLLs loaded successfully!");
            return true;
        }

        // Unload all loaded DLLs (in reverse order)
        void UnloadAll()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            UnloadAllInternal();
        }

        bool IsLoaded() const { return m_loaded; }

    private:
        DllLoader() = default;
        ~DllLoader() { UnloadAllInternal(); }

        DllLoader(const DllLoader&) = delete;
        DllLoader& operator=(const DllLoader&) = delete;

        void UnloadAllInternal()
        {
            for (auto it = m_loadedModules.rbegin(); it != m_loadedModules.rend(); ++it) {
                if (*it) {
                    FreeLibrary(*it);
                }
            }
            m_loadedModules.clear();
            m_loaded = false;
        }

        std::vector<HMODULE> m_loadedModules;
        bool m_loaded = false;
        mutable std::mutex m_mutex;
    };
}
