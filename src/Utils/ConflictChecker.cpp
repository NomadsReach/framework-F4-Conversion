#include "ConflictChecker.h"

#include <Psapi.h>
#include <algorithm>
#include <cwctype>
#include <dxgi.h>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

#pragma comment(lib, "Psapi.lib")
#pragma intrinsic(_ReturnAddress)

namespace PrismaUI::ConflictChecker {
    static std::wstring GetModuleBasenameW(HMODULE module) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring full(path);
        auto pos = full.rfind(L'\\');
        return pos != std::wstring::npos ? full.substr(pos + 1) : full;
    }

    static std::string WideToUtf8(const std::wstring& value) {
        if (value.empty()) {
            return {};
        }

        int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr,
                                      nullptr);
        if (len <= 0) {
            return {};
        }

        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len, nullptr,
                            nullptr);
        return out;
    }

    static std::string WideToUtf8(const wchar_t* value) {
        return value ? WideToUtf8(std::wstring(value)) : std::string{};
    }

    static std::string OwnerOf(void* address) {
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &owner) &&
            owner) {
            return WideToUtf8(GetModuleBasenameW(owner));
        }
        return "<unknown>";
    }

    void CheckEarly() {
        logger::info("[ConflictChecker] CheckEarly: scanning loaded modules");

        HANDLE process = GetCurrentProcess();
        HMODULE modules[1024] = {};
        DWORD bytesNeeded = 0;

        if (!EnumProcessModules(process, modules, sizeof(modules), &bytesNeeded)) {
            logger::warn("[ConflictChecker] CheckEarly: EnumProcessModules failed (error {})", GetLastError());
        } else {
            const DWORD count = bytesNeeded / sizeof(HMODULE);
            std::vector<std::wstring> matches;

            for (DWORD i = 0; i < count; ++i) {
                std::wstring basename = GetModuleBasenameW(modules[i]);
                std::wstring lower = basename;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (lower == L"prismaui_f4.dll") {
                    wchar_t path[MAX_PATH] = {};
                    GetModuleFileNameW(modules[i], path, MAX_PATH);
                    matches.emplace_back(path);
                }
            }

            if (matches.size() == 1) {
                logger::info("[ConflictChecker] Module check OK: single instance at '{}'", WideToUtf8(matches[0]));
            } else if (matches.size() > 1) {
                logger::critical("[ConflictChecker] DUPLICATE: PrismaUI_F4.dll loaded {} times:", matches.size());
                for (const auto& path : matches) {
                    logger::critical("[ConflictChecker]   -> '{}'", WideToUtf8(path));
                }
            } else {
                logger::warn("[ConflictChecker] CheckEarly: PrismaUI_F4.dll not found in module list (unexpected)");
            }
        }

        HMODULE ours = GetModuleHandleW(L"PrismaUI_F4.dll");
        if (!ours) {
            logger::warn("[ConflictChecker] CheckEarly: GetModuleHandleW(PrismaUI_F4.dll) returned null");
            return;
        }

        void* exportFn = reinterpret_cast<void*>(GetProcAddress(ours, "RequestPluginAPI"));
        if (!exportFn) {
            logger::warn("[ConflictChecker] CheckEarly: GetProcAddress(RequestPluginAPI) returned null");
            return;
        }

        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(exportFn), &owner);

        if (owner && owner != ours) {
            logger::critical(
                "[ConflictChecker] CONFLICT: RequestPluginAPI export resolves to '{}' instead of our module - another "
                "DLL is squatting on our name",
                WideToUtf8(GetModuleBasenameW(owner)));
        } else {
            logger::info("[ConflictChecker] Export check OK: RequestPluginAPI owned by PrismaUI_F4.dll");
        }
    }

    static constexpr const wchar_t* kKnownConflictDlls[] = {
        L"ReShade.dll",
        L"ReShade64.dll",
        L"FallSouls.dll",
        L"FallSouls_NG.dll",
        L"enbseries.dll",
        L"dxvk.dll",
        L"d3d11_log.dll",
    };

    struct APIConsumer {
        std::string name;
        PRISMA_UI_API::InterfaceVersion version;
    };

    static std::vector<APIConsumer> apiConsumers;
    static std::mutex apiConsumersMutex;

    static void PrintAPIConsumerSummary() {
        std::lock_guard lock(apiConsumersMutex);
        if (apiConsumers.empty()) {
            logger::warn("[ConflictChecker] No Prisma plugins connected via RequestPluginAPI by kGameDataReady");
            return;
        }

        std::string summary;
        for (size_t i = 0; i < apiConsumers.size(); ++i) {
            if (i > 0) {
                summary += ", ";
            }
            summary += apiConsumers[i].name;
            summary += " (V";
            summary += std::to_string(static_cast<int>(apiConsumers[i].version));
            summary += ")";
        }
        logger::info("[ConflictChecker] Prisma API consumers ({}): {}", apiConsumers.size(), summary);
    }

    void CheckPreHooks() {
        logger::info("[ConflictChecker] CheckPreHooks: scanning for D3D hook conflicts");

        bool anyKnownConflict = false;
        for (const auto* dllName : kKnownConflictDlls) {
            if (GetModuleHandleW(dllName)) {
                logger::warn("[ConflictChecker] WARNING: '{}' is loaded - may conflict with D3D Present hook",
                             WideToUtf8(dllName));
                anyKnownConflict = true;
            }
        }
        if (!anyKnownConflict) {
            logger::info("[ConflictChecker] Known-DLL scan OK: no known conflicting modules detected");
        }

        auto* rendererData = RE::BSGraphics::GetRendererData();
        if (!rendererData || !rendererData->renderWindow[0].swapChain) {
            logger::warn("[ConflictChecker] CheckPreHooks: cannot get IDXGISwapChain for vtable check - skipping");
            PrintAPIConsumerSummary();
            return;
        }

        auto* swapChain = rendererData->renderWindow[0].swapChain;
        void** vtable = *reinterpret_cast<void***>(swapChain);

        uintptr_t dxgiBase = 0;
        uintptr_t dxgiEnd = 0;
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (dxgi) {
            MODULEINFO info = {};
            if (GetModuleInformation(GetCurrentProcess(), dxgi, &info, sizeof(info))) {
                dxgiBase = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
                dxgiEnd = dxgiBase + info.SizeOfImage;
            }
        }

        auto checkVtableEntry = [&](int index, const char* name) {
            uintptr_t address = reinterpret_cast<uintptr_t>(vtable[index]);
            const bool inDxgi = dxgiBase != 0 && address >= dxgiBase && address < dxgiEnd;
            if (inDxgi) {
                logger::info("[ConflictChecker] vtable[{}] ({}) OK - points inside dxgi.dll", index, name);
                return;
            }

            logger::error(
                "[ConflictChecker] CONFLICT: vtable[{}] ({}) already hooked by '{}' at 0x{:016X} - rendering may "
                "be unstable",
                index, name, OwnerOf(vtable[index]), address);
        };

        checkVtableEntry(8, "Present");
        checkVtableEntry(13, "ResizeBuffers");
        PrintAPIConsumerSummary();
    }

    void OnAPIRequest(void* returnAddress, PRISMA_UI_API::InterfaceVersion version) {
        std::string callerName = "<unknown>";
        HMODULE caller = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(returnAddress), &caller) &&
            caller) {
            callerName = WideToUtf8(GetModuleBasenameW(caller));
        }

        const int versionNum = static_cast<int>(version);
        logger::info("[ConflictChecker] API request: caller='{}' requested V{}", callerName, versionNum);

        const bool supported = version == PRISMA_UI_API::InterfaceVersion::V1 ||
                               version == PRISMA_UI_API::InterfaceVersion::V2 ||
                               version == PRISMA_UI_API::InterfaceVersion::V3 ||
                               version == PRISMA_UI_API::InterfaceVersion::V4;
        if (!supported) {
            logger::error(
                "[ConflictChecker] CONFLICT: '{}' requested unsupported interface version V{} - plugin likely built "
                "against a different PrismaUI_F4",
                callerName, versionNum);
        }

        std::lock_guard lock(apiConsumersMutex);
        for (const auto& consumer : apiConsumers) {
            if (consumer.name == callerName) {
                return;
            }
        }
        apiConsumers.push_back({callerName, version});
    }
}
