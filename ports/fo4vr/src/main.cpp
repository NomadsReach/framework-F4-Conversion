#include "PCH.h"

#include "Hooks/Hooks.h"
#include "Menus/FocusMenu/FocusMenu.h"
#include "PrismaUI/SceneDepthCapture.h"
#include "Utils/DllLoader.h"

namespace
{
    std::atomic<bool> g_runtimeInstalled = false;

    [[nodiscard]] bool IsExactSupportedRuntime() noexcept
    {
        return REL::Module::IsVR() &&
               REL::Module::get().version() ==
                   F4SE::RUNTIME_VR_1_2_72;
    }

    void F4SEAPI HandleMessage(
        F4SE::MessagingInterface::Message* message)
    {
        if (!message ||
            message->type !=
                F4SE::MessagingInterface::kGameDataReady ||
            g_runtimeInstalled.exchange(
                true,
                std::memory_order_acq_rel)) {
            return;
        }

        try {
            const auto ui = RE::UI::GetSingleton();
            if (!ui) {
                logger::critical(
                    "PrismaUI could not register its focus menu");
                return;
            }
            ui->RegisterMenu(
                FocusMenu::MENU_NAME.data(),
                FocusMenu::Creator);

            if (!Hooks::EngineVRHooks::Install()) {
                logger::critical(
                    "PrismaUI could not install the required stereo submission boundary");
                return;
            }
            if (!PrismaUI::SceneDepthCapture::Install()) {
                logger::warn(
                    "PrismaUI scene-depth occlusion is unavailable");
            }
            if (!Hooks::D3DHooks::Install()) {
                logger::warn(
                    "PrismaUI swap-chain resize invalidation is unavailable");
            }
            logger::info(
                "PrismaUI FO4VR runtime integration is ready");
        } catch (...) {
            logger::critical(
                "PrismaUI runtime integration failed");
        }
    }
}

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData version{};
    version.PluginVersion({1, 0, 0, 0});
    version.PluginName("PrismaUI_F4");
    version.AuthorName("PrismaUI");
    version.UsesAddressLibrary(true);
    version.UsesSigScanning(false);
    version.IsLayoutDependent(true);
    version.HasNoStructUse(false);
    version.CompatibleVersions({
        F4SE::RUNTIME_VR_1_2_72
    });
    return version;
}();

F4SE_EXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* f4se,
    F4SE::PluginInfo* info)
{
    if (!f4se || !info) {
        return false;
    }
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = "PrismaUI_F4";
    info->version =
        REL::Version(1, 0, 0, 0).pack();
    // F4SEVR 0.6.21 exposes its legacy 1.10.138 compatibility
    // value through QueryInterface::RuntimeVersion(). Validate
    // the actual executable instead.
    return !f4se->IsEditor() &&
           IsExactSupportedRuntime();
}

F4SE_PLUGIN_LOAD(
    const F4SE::LoadInterface* f4se)
{
    if (!f4se) {
        return false;
    }
    F4SE::Init(f4se);
    if (!IsExactSupportedRuntime()) {
        logger::critical(
            "PrismaUI requires Fallout 4 VR 1.2.72");
        return false;
    }

    F4SE::AllocTrampoline(4096);
    if (!PrismaUI::Utils::DllLoader::GetSingleton().
            LoadUltralightLibraries()) {
        logger::critical(
            "PrismaUI could not load the staged Ultralight 1.4.0 runtime");
        return false;
    }

    const auto messaging =
        F4SE::GetMessagingInterface();
    if (!messaging ||
        !messaging->RegisterListener(HandleMessage)) {
        logger::critical(
            "PrismaUI could not register its F4SE message listener");
        return false;
    }

    logger::info(
        "PrismaUI_F4 FO4VR provider loaded");
    return true;
}
