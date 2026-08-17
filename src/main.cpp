#include "PCH.h"

#include <array>
#include <commctrl.h>
#include <filesystem>
#include <intrin.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "API/API.h"
#include "Hooks/Hooks.h"
#include "PrismaUI/NotificationSystem.h"
#include "Utils/ConflictChecker.h"
#include "Utils/DllLoader.h"

static bool g_overlayDetected = false;
static bool g_pluginDisabled = false;
static std::string g_detectedOverlayName;

namespace {
    std::string GetIniPath() {
        wchar_t modPath[MAX_PATH] = {};
        GetModuleFileNameW(GetModuleHandleW(L"PrismaUI_F4.dll"), modPath, MAX_PATH);
        auto ini = std::filesystem::path(modPath).replace_extension(L".ini");
        return ini.string();
    }

    bool ReadIniBool(const char* section, const char* key, bool defaultValue) {
        const std::string path = GetIniPath();
        const int defaultInt = defaultValue ? 1 : 0;
        return GetPrivateProfileIntA(section, key, defaultInt, path.c_str()) != 0;
    }

    struct OverlayProcess {
        const char* executable;
        const char* displayName;
    };

    constexpr std::array<OverlayProcess, 5> kConflictingOverlays = {{
        {"RTSS.exe", "RivaTuner Statistics Server (RTSS.exe)"},
        {"RTSSHooked.exe", "RivaTuner (RTSSHooked.exe)"},
        {"RTSSHooksLoader64.exe", "RivaTuner Loader (RTSSHooksLoader64.exe) - close from system tray"},
        {"MSIAfterburner.exe", "MSI Afterburner (MSIAfterburner.exe)"},
        {"OverdriveNTool.exe", "AMD Overdrive (OverdriveNTool.exe)"},
    }};

    bool IsConflictingOverlayRunning() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }

        PROCESSENTRY32 entry{};
        entry.dwSize = sizeof(PROCESSENTRY32);

        if (Process32First(snapshot, &entry)) {
            do {
                for (const auto& overlay : kConflictingOverlays) {
                    if (_stricmp(entry.szExeFile, overlay.executable) != 0) {
                        continue;
                    }

                    logger::warn("[PrismaUI Overlay Detection] Found: {}", entry.szExeFile);
                    g_detectedOverlayName = overlay.displayName;
                    CloseHandle(snapshot);
                    return true;
                }
            } while (Process32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return false;
    }

    void NotifyUserOverlayDetected() {
        std::wstring overlayW(g_detectedOverlayName.begin(), g_detectedOverlayName.end());
        std::wstring content =
            L"Detected: " + overlayW + L"\n\n"
            L"This software hooks DirectX and can conflict with Ultralight rendering.\n"
            L"Click Continue to load anyway - PrismaUI will remember this choice.";

        TASKDIALOGCONFIG dialog = {};
        dialog.cbSize = sizeof(dialog);
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.pszWindowTitle = L"PrismaUI Framework";
        dialog.pszMainIcon = TD_WARNING_ICON;
        dialog.pszMainInstruction = L"GPU Overlay Software Detected";
        dialog.pszContent = content.c_str();
        dialog.dwCommonButtons = TDCBF_OK_BUTTON;
        dialog.nDefaultButton = IDOK;
        dialog.pszFooter = L"How to disable the overlay: youtube.com/watch?v=1NPqDMlYGz0";
        dialog.pszFooterIcon = TD_INFORMATION_ICON;

        // OG Fallout4.exe may not provide the comctl32 v6 activation context TaskDialogIndirect requires.
        ACTCTXW actCtx = {};
        actCtx.cbSize = sizeof(actCtx);
        actCtx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
        actCtx.lpResourceName = MAKEINTRESOURCEW(2);
        actCtx.hModule = GetModuleHandleW(L"PrismaUI_F4.dll");

        HANDLE activationContext = CreateActCtxW(&actCtx);
        ULONG_PTR cookie = 0;
        const bool activated = activationContext != INVALID_HANDLE_VALUE && ActivateActCtx(activationContext, &cookie);
        if (!activated) {
            logger::warn("[PrismaUI] Could not activate comctl32 v6 context (hCtx={}, GLE={})",
                         static_cast<void*>(activationContext), GetLastError());
        }

        using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        TaskDialogIndirectFn taskDialogIndirect = nullptr;
        HMODULE comCtl = LoadLibraryW(L"comctl32.dll");
        if (comCtl) {
            taskDialogIndirect =
                reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(comCtl, "TaskDialogIndirect"));
        }

        logger::info("[PrismaUI] overlay dialog: activated={} pfn={}", activated,
                     reinterpret_cast<void*>(taskDialogIndirect));

        int button = 0;
        HRESULT result = E_NOTIMPL;
        if (taskDialogIndirect) {
            result = taskDialogIndirect(&dialog, &button, nullptr, nullptr);
        }

        logger::info("[PrismaUI] TaskDialogIndirect hr=0x{:08x} nButton={}", static_cast<uint32_t>(result), button);

        if (comCtl) {
            FreeLibrary(comCtl);
        }
        if (activated) {
            DeactivateActCtx(0, cookie);
        }
        if (activationContext != INVALID_HANDLE_VALUE) {
            ReleaseActCtx(activationContext);
        }

        if (FAILED(result) || button == 0) {
            logger::warn("[PrismaUI] TaskDialogIndirect unavailable (hr=0x{:08x} nButton={}), using MessageBoxW",
                         static_cast<uint32_t>(result), button);
            std::wstring fallback =
                L"GPU overlay software detected: " + overlayW + L"\n\n"
                L"This software hooks DirectX and can conflict with Ultralight rendering.\n\n"
                L"Click OK to continue loading with overlay support enabled.\n"
                L"You may see visual artifacts - close the overlay if issues occur.\n\n"
                L"How to disable the RTSS overlay:\n"
                L"https://www.youtube.com/watch?v=1NPqDMlYGz0";
            MessageBoxW(nullptr, fallback.c_str(), L"PrismaUI - Overlay Detected", MB_OK | MB_ICONWARNING);
        }

        const std::string iniPath = GetIniPath();
        WritePrivateProfileStringA("Compatibility", "bAllowOverlays", "1", iniPath.c_str());
        logger::warn("[PrismaUI] bAllowOverlays=1 written to {} - overlay check suppressed on next launch", iniPath);
    }
}

static void F4SEMessageHandler(F4SE::MessagingInterface::Message* message) {
    if (message->type != F4SE::MessagingInterface::kGameDataReady) {
        return;
    }

    PrismaUI::ConflictChecker::CheckPreHooks();
    Hooks::D3DHooks::Install();
}

// One version export serves OG, NG, and AE; this target links CommonLibF4 directly.
F4SE_PLUGIN_VERSION = []() noexcept {
    F4SE::PluginVersionData version{};
    version.PluginVersion({1, 0, 0, 0});
    version.PluginName("PrismaUI_F4");
    version.AuthorName("PrismaUI");
    version.UsesAddressLibrary(true);
    version.UsesAddressLibraryNG(true);
    version.UsesSigScanning(false);
    version.IsLayoutDependent(true);
    version.IsLayoutDependentNG(true);
    version.HasNoStructUse(false);
    return version;
}();

// Old-Gen 1.10.163 uses the legacy query export.
F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* f4se, F4SE::PluginInfo* info) {
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = "PrismaUI_F4";
    info->version = 1;

    if (f4se->IsEditor()) {
        return false;
    }
    return f4se->RuntimeVersion() >= F4SE::RUNTIME_1_10_162;
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* loadInterface) {
    F4SE::Init(loadInterface, F4SE::InitInfo{
                                  .logName = "PrismaUI_F4",
                                  .trampoline = true,
                                  .trampolineSize = 1 << 10,
                              });

    PrismaUI::ConflictChecker::CheckEarly();

    g_overlayDetected = IsConflictingOverlayRunning();
    if (g_overlayDetected) {
        const bool allowOverlays = ReadIniBool("Compatibility", "bAllowOverlays", false);
        logger::warn("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        logger::warn("[PrismaUI] Overlay detected: {}", g_detectedOverlayName);
        if (allowOverlays) {
            logger::warn("[PrismaUI] bAllowOverlays=1 in PrismaUI_F4.ini - continuing anyway");
            logger::warn("[PrismaUI] Rendering artifacts or crashes may occur. Use at your own risk.");
            logger::warn("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        } else {
            logger::warn("[PrismaUI] Set bAllowOverlays=1 in PrismaUI_F4.ini to suppress this warning.");
            logger::warn("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            NotifyUserOverlayDetected();
        }
    }

    const auto* messaging = F4SE::GetMessagingInterface();
    if (!messaging) {
        logger::critical("Failed to get F4SE messaging interface!");
        return false;
    }
    messaging->RegisterListener(F4SEMessageHandler);

    if (!PrismaUI::Utils::DllLoader::GetSingleton().LoadUltralightLibraries()) {
        logger::critical("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        logger::critical("[PrismaUI CRITICAL] Failed to load Ultralight libraries!");
        logger::critical("Required files missing from: Data/PrismaUI_F4/libs/");
        logger::critical("  AppCore.dll, Ultralight.dll, UltralightCore.dll, WebCore.dll");
        logger::critical("Also check Data/PrismaUI_F4/resources/: cacert.pem, icudt67l.dat");
        logger::critical("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        return false;
    }

    logger::info("PrismaUI_F4 Framework loaded successfully");
    return true;
}

extern "C" __declspec(dllexport) void* F4SEAPI
RequestPluginAPI(const PRISMA_UI_API::InterfaceVersion interfaceVersion) {
    PrismaUI::ConflictChecker::OnAPIRequest(_ReturnAddress(), interfaceVersion);

    if (g_pluginDisabled) {
        logger::warn("RequestPluginAPI: PrismaUI is disabled (GPU overlay detected)");
        return nullptr;
    }

    auto api = PluginAPI::PrismaUIInterface::GetSingleton();
    switch (interfaceVersion) {
        case PRISMA_UI_API::InterfaceVersion::V1:
            logger::info("RequestPluginAPI returned V1 interface");
            return static_cast<PRISMA_UI_API::IVPrismaUI1*>(api);
        case PRISMA_UI_API::InterfaceVersion::V2:
            logger::info("RequestPluginAPI returned V2 interface");
            return static_cast<PRISMA_UI_API::IVPrismaUI2*>(api);
        case PRISMA_UI_API::InterfaceVersion::V3:
            logger::info("RequestPluginAPI returned V3 interface");
            return static_cast<PRISMA_UI_API::IVPrismaUI3*>(api);
        case PRISMA_UI_API::InterfaceVersion::V4:
            logger::info("RequestPluginAPI returned V4 interface");
            return static_cast<PRISMA_UI_API::IVPrismaUI4*>(api);
        default:
            logger::info("RequestPluginAPI: unsupported interface version");
            return nullptr;
    }
}
