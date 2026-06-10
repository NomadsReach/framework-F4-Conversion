#include "PCH.h"
#include "API/API.h"
#include "Hooks/Hooks.h"
#include "Utils/DllLoader.h"
#include "PrismaUI/NotificationSystem.h"
#include <tlhelp32.h>
#include <filesystem>
#include <commctrl.h>
#include <shellapi.h>

static bool g_overlayDetected = false;
static bool g_pluginDisabled = false;
static std::string g_detectedOverlayName;

namespace {
    // Returns the path to Data\F4SE\Plugins\PrismaUI_F4.ini by deriving it
    // from the DLL's own location (the DLL lives in that same folder).
    static std::string GetIniPath() {
        wchar_t modPath[MAX_PATH] = {};
        GetModuleFileNameW(GetModuleHandleW(L"PrismaUI_F4.dll"), modPath, MAX_PATH);
        auto ini = std::filesystem::path(modPath).replace_extension(L".ini");
        return ini.string();
    }

    // Read a bool (0/1) from [section] key in PrismaUI_F4.ini.
    // Returns defaultValue when the file or key is absent.
    static bool ReadIniBool(const char* section, const char* key, bool defaultValue) {
        const std::string path = GetIniPath();
        const int def = defaultValue ? 1 : 0;
        const int val = GetPrivateProfileIntA(section, key, def, path.c_str());
        return val != 0;
    }
}

namespace {
    bool IsConflictingOverlayRunning() {
        const char* conflicting_procs[] = {
            "RTSS.exe",              // RivaTuner Statistics Server
            "RTSSHooked.exe",        // RivaTuner hooked version
            "RTSSHooksLoader64.exe", // RivaTuner background loader (even when window closed)
            "MSIAfterburner.exe",    // MSI Afterburner
            "OverdriveNTool.exe",    // AMD Overdrive
        };

        const char* friendly_names[] = {
            "RivaTuner Statistics Server (RTSS.exe)",
            "RivaTuner (RTSSHooked.exe)",
            "RivaTuner Loader (RTSSHooksLoader64.exe) - close from system tray",
            "MSI Afterburner (MSIAfterburner.exe)",
            "AMD Overdrive (OverdriveNTool.exe)",
        };

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;

        PROCESSENTRY32 entry{};
        entry.dwSize = sizeof(PROCESSENTRY32);

        if (Process32First(snapshot, &entry)) {
            do {
                for (size_t i = 0; i < 5; ++i) {
                    if (_stricmp(entry.szExeFile, conflicting_procs[i]) == 0) {
                        logger::warn("[PrismaUI Overlay Detection] Found: {}", entry.szExeFile);
                        g_detectedOverlayName = friendly_names[i];
                        CloseHandle(snapshot);
                        return true;
                    }
                }
            } while (Process32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return false;
    }

    static HRESULT CALLBACK OverlayDialogCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lParam, LONG_PTR) {
        if (msg == TDN_HYPERLINK_CLICKED)
            ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
        return S_OK;
    }

    void NotifyUserOverlayDetected() {
        // Convert ASCII overlay name to wide for TaskDialog
        std::wstring overlayW(g_detectedOverlayName.begin(), g_detectedOverlayName.end());

        std::wstring content =
            L"Detected: " + overlayW + L"\n\n"
            L"This software hooks DirectX and can conflict with Ultralight rendering.\n"
            L"Click Continue to load anyway - PrismaUI will remember this choice.";

        TASKDIALOG_BUTTON btn = {
            IDOK,
            L"Continue loading\n"
            L"PrismaUI will load alongside the overlay. Close it if you see rendering issues."
        };

        TASKDIALOGCONFIG tdc      = {};
        tdc.cbSize                = sizeof(tdc);
        tdc.dwFlags               = TDF_ENABLE_HYPERLINKS | TDF_USE_COMMAND_LINKS |
                                    TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        tdc.pszWindowTitle        = L"PrismaUI Framework";
        tdc.pszMainIcon           = TD_WARNING_ICON;
        tdc.pszMainInstruction    = L"GPU Overlay Software Detected";
        tdc.pszContent            = content.c_str();
        tdc.pButtons              = &btn;
        tdc.cButtons              = 1;
        tdc.nDefaultButton        = IDOK;
        tdc.pszFooter             =
            L"<a href=\"https://www.youtube.com/watch?v=1NPqDMlYGz0\">"
            L"How to disable the RTSS overlay (video guide)"
            L"</a>";
        tdc.pszFooterIcon         = TD_INFORMATION_ICON;
        tdc.pfCallback            = OverlayDialogCallback;

        TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);

        // Persist the opt-in so this dialog never appears again
        const std::string iniPath = GetIniPath();
        WritePrivateProfileStringA("Compatibility", "bAllowOverlays", "1", iniPath.c_str());
        logger::warn("[PrismaUI] bAllowOverlays=1 written to {} - overlay check suppressed on next launch", iniPath);
    }
}

static void F4SEMessageHandler(F4SE::MessagingInterface::Message* message)
{
    if (message->type == F4SE::MessagingInterface::kGameDataReady) {
        Hooks::D3DHooks::Install();
    }
}

// F4SE plugin version data - required for the NG/AE loader to recognise the DLL.
// This target links commonlibf4 directly (it does NOT use the commonlibf4.plugin
// rule), so the version export is declared here rather than auto-generated.
// One DLL serves OG + NG + AE (COMMONLIB_RUNTIMECOUNT=3).
F4SE_PLUGIN_VERSION = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ 1, 0, 0, 0 });
    v.PluginName("PrismaUI_F4");
    v.AuthorName("PrismaUI");
    v.UsesAddressLibrary(true);     // 1.11.137+ (AE) address library
    v.UsesAddressLibraryNG(true);   // 1.10.980/984 (NG) address library
    v.UsesSigScanning(false);
    v.IsLayoutDependent(true);
    v.IsLayoutDependentNG(true);
    v.HasNoStructUse(false);
    return v;
}();

// F4SEPlugin_Query - used by the Old-Gen (1.10.163) loader, which predates the
// version-data export above. Both are exported so one DLL loads on OG and NG.
F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = "PrismaUI_F4";
    a_info->version = 1;

    if (a_f4se->IsEditor()) {
        return false;
    }
    return a_f4se->RuntimeVersion() >= F4SE::RUNTIME_1_10_162;
}

// F4SE::Init() creates the log file automatically at:
//   %USERPROFILE%\Documents\My Games\Fallout4\F4SE\PrismaUI_F4.log
F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_intfc)
{
    F4SE::Init(a_intfc, F4SE::InitInfo{
        .logName        = "PrismaUI_F4",
        .trampoline     = true,
        .trampolineSize = 1 << 10
    });

    g_overlayDetected = IsConflictingOverlayRunning();
    if (g_overlayDetected) {
        const bool allowOverlays = ReadIniBool("Compatibility", "bAllowOverlays", false);
        logger::warn("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        logger::warn("[PrismaUI] Overlay detected: {}", g_detectedOverlayName);
        if (allowOverlays) {
            logger::warn("[PrismaUI] bAllowOverlays=1 in PrismaUI_F4.ini — continuing anyway");
            logger::warn("[PrismaUI] Rendering artifacts or crashes may occur. Use at your own risk.");
            logger::warn("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        } else {
            logger::warn("[PrismaUI] Exiting. Set bAllowOverlays=1 in PrismaUI_F4.ini to suppress this.");
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

// Exported entry point that every PrismaUI client plugin resolves via
// GetProcAddress(GetModuleHandleW(L"PrismaUI_F4.dll"), "RequestPluginAPI").
extern "C" __declspec(dllexport) void* F4SEAPI RequestPluginAPI(const PRISMA_UI_API::InterfaceVersion a_interfaceVersion)
{
    if (g_pluginDisabled) {
        logger::warn("RequestPluginAPI: PrismaUI is disabled (GPU overlay detected)");
        return nullptr;
    }

    auto api = PluginAPI::PrismaUIInterface::GetSingleton();

    switch (a_interfaceVersion) {
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
