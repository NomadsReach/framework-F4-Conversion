#include "PCH.h"

#include <commctrl.h>
#include <filesystem>
#include <shellapi.h>
#include <tlhelp32.h>

#include "API/API.h"
#include "Hooks/Hooks.h"
#include "PrismaUI/GameThreadDispatcher.h"
#include "PrismaUI/NotificationSystem.h"
#include "Utils/DllLoader.h"

static bool g_overlayDetected = false;
static std::string g_detectedOverlayName;

namespace {

std::string GetIniPath()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(GetModuleHandleW(L"PrismaUI_F4.dll"), modulePath, MAX_PATH)) return {};
    return std::filesystem::path(modulePath).replace_extension(L".ini").string();
}

bool ReadIniBool(const char* section, const char* key, bool defaultValue)
{
    const std::string path = GetIniPath();
    if (path.empty()) return defaultValue;
    return GetPrivateProfileIntA(section, key, defaultValue ? 1 : 0, path.c_str()) != 0;
}

bool IsConflictingOverlayRunning()
{
    constexpr const char* processes[] = {
        "RTSS.exe",
        "RTSSHooked.exe",
        "RTSSHooksLoader64.exe",
        "MSIAfterburner.exe",
        "OverdriveNTool.exe",
    };
    constexpr const char* names[] = {
        "RivaTuner Statistics Server (RTSS.exe)",
        "RivaTuner (RTSSHooked.exe)",
        "RivaTuner Loader (RTSSHooksLoader64.exe) - close from system tray",
        "MSI Afterburner (MSIAfterburner.exe)",
        "AMD Overdrive (OverdriveNTool.exe)",
    };

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            for (size_t i = 0; i < std::size(processes); ++i) {
                if (_stricmp(entry.szExeFile, processes[i]) == 0) {
                    g_detectedOverlayName = names[i];
                    CloseHandle(snapshot);
                    return true;
                }
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return false;
}

void NotifyUserOverlayDetected()
{
    const std::wstring overlayName(g_detectedOverlayName.begin(), g_detectedOverlayName.end());
    const std::wstring content =
        L"Detected: " + overlayName + L"\n\n"
        L"This software hooks DirectX and can conflict with Ultralight rendering.\n"
        L"Click OK to continue. PrismaUI will remember this choice.";

    TASKDIALOGCONFIG dialog{};
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

    ACTCTXW activation{};
    activation.cbSize = sizeof(activation);
    activation.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
    activation.lpResourceName = MAKEINTRESOURCEW(2);
    activation.hModule = GetModuleHandleW(L"PrismaUI_F4.dll");

    HANDLE context = CreateActCtxW(&activation);
    ULONG_PTR cookie = 0;
    const bool active = context != INVALID_HANDLE_VALUE && ActivateActCtx(context, &cookie);

    using TaskDialogFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    TaskDialogFn taskDialog = comctl ? reinterpret_cast<TaskDialogFn>(GetProcAddress(comctl, "TaskDialogIndirect"))
                                     : nullptr;

    int button = 0;
    HRESULT result = E_NOTIMPL;
    if (taskDialog) result = taskDialog(&dialog, &button, nullptr, nullptr);

    if (comctl) FreeLibrary(comctl);
    if (active) DeactivateActCtx(0, cookie);
    if (context != INVALID_HANDLE_VALUE) ReleaseActCtx(context);

    if (FAILED(result) || button == 0) {
        const std::wstring fallback =
            L"GPU overlay software detected: " + overlayName + L"\n\n"
            L"This software hooks DirectX and can conflict with Ultralight rendering.\n\n"
            L"Click OK to continue. Close the overlay if rendering problems occur.\n\n"
            L"https://www.youtube.com/watch?v=1NPqDMlYGz0";
        MessageBoxW(nullptr, fallback.c_str(), L"PrismaUI - Overlay Detected", MB_OK | MB_ICONWARNING);
    }

    const std::string iniPath = GetIniPath();
    if (!iniPath.empty()) WritePrivateProfileStringA("Compatibility", "bAllowOverlays", "1", iniPath.c_str());
}

void F4SEMessageHandler(F4SE::MessagingInterface::Message* message)
{
    if (message && message->type == F4SE::MessagingInterface::kGameDataReady) {
        Hooks::D3DHooks::Install();
    }
}

}

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

F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* f4se, F4SE::PluginInfo* info)
{
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = "PrismaUI_F4";
    info->version = 1;
    if (f4se->IsEditor()) return false;
    return f4se->RuntimeVersion() >= F4SE::RUNTIME_1_10_162;
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* f4se)
{
    F4SE::Init(f4se, F4SE::InitInfo{
        .logName = "PrismaUI_F4",
        .trampoline = true,
        .trampolineSize = 1 << 10,
    });

    PrismaUI::GameThreadDispatcher::CaptureCurrentThread();

    g_overlayDetected = IsConflictingOverlayRunning();
    if (g_overlayDetected) {
        const bool allowOverlays = ReadIniBool("Compatibility", "bAllowOverlays", false);
        logger::warn("[PrismaUI] Overlay detected: {}", g_detectedOverlayName);
        if (!allowOverlays) {
            logger::warn("[PrismaUI] Showing one-time overlay warning; loading continues after acknowledgement.");
            NotifyUserOverlayDetected();
        }
    }

    const auto* messaging = F4SE::GetMessagingInterface();
    if (!messaging) {
        logger::critical("Failed to get F4SE messaging interface");
        return false;
    }
    messaging->RegisterListener(F4SEMessageHandler);

    if (!PrismaUI::Utils::DllLoader::GetSingleton().LoadUltralightLibraries()) {
        logger::critical("Failed to load Ultralight libraries from Data/PrismaUI_F4/libs");
        return false;
    }

    logger::info("PrismaUI_F4 Framework loaded successfully");
    return true;
}

extern "C" __declspec(dllexport) void* F4SEAPI RequestPluginAPI(PRISMA_UI_API::InterfaceVersion version)
{
    auto* api = PluginAPI::PrismaUIInterface::GetSingleton();
    switch (version) {
        case PRISMA_UI_API::InterfaceVersion::V1:
            return static_cast<PRISMA_UI_API::IVPrismaUI1*>(api);
        case PRISMA_UI_API::InterfaceVersion::V2:
            return static_cast<PRISMA_UI_API::IVPrismaUI2*>(api);
        case PRISMA_UI_API::InterfaceVersion::V3:
            return static_cast<PRISMA_UI_API::IVPrismaUI3*>(api);
        case PRISMA_UI_API::InterfaceVersion::V4:
            return static_cast<PRISMA_UI_API::IVPrismaUI4*>(api);
        default:
            return nullptr;
    }
}
