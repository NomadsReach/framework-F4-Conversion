#pragma once

#include <Windows.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace PrismaUI::Utils {
    inline std::filesystem::path GetBasePath() {
        return std::filesystem::current_path() / "Data" / "PrismaUI_F4";
    }

    class DllLoader {
    public:
        static DllLoader& GetSingleton() {
            static DllLoader instance;
            return instance;
        }

        bool LoadUltralightLibraries() {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_loaded) {
                return true;
            }

            auto libsPath = GetBasePath() / "libs";
            if (!std::filesystem::exists(libsPath)) {
                logger::error("Ultralight libs path does not exist: {}", libsPath.string());
                return false;
            }

            // Ultralight DLL dependency order is significant.
            const std::vector<std::wstring> dllNames = {
                L"UltralightCore.dll",
                L"WebCore.dll",
                L"Ultralight.dll",
                L"AppCore.dll",
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

        void UnloadAll() {
            std::lock_guard<std::mutex> lock(m_mutex);
            UnloadAllInternal();
        }

        bool IsLoaded() const { return m_loaded; }

    private:
        DllLoader() = default;
        ~DllLoader() { UnloadAllInternal(); }

        DllLoader(const DllLoader&) = delete;
        DllLoader& operator=(const DllLoader&) = delete;

        void UnloadAllInternal() {
            if (!m_loadedModules.empty()) {
                logger::info("DllLoader: unloading {} Ultralight DLL(s) in reverse order", m_loadedModules.size());
            }

            for (auto it = m_loadedModules.rbegin(); it != m_loadedModules.rend(); ++it) {
                if (!*it) {
                    continue;
                }

                char modName[MAX_PATH] = {};
                GetModuleFileNameA(*it, modName, MAX_PATH);
                FreeLibrary(*it);
                logger::info("DllLoader: unloaded '{}'", modName[0] ? modName : "<unknown>");
            }

            m_loadedModules.clear();
            m_loaded = false;
        }

        std::vector<HMODULE> m_loadedModules;
        bool m_loaded = false;
        mutable std::mutex m_mutex;
    };
}
