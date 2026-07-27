#include "PCH.h"

#include "PrismaUI_F4_API.h"
#include "PrismaUI_F4VR_API.h"

namespace
{
    std::atomic<bool> g_resolved = false;

    void F4SEAPI HandleMessage(
        F4SE::MessagingInterface::Message* message)
    {
        if (!message ||
            message->type !=
                F4SE::MessagingInterface::kGameDataReady ||
            g_resolved.exchange(
                true,
                std::memory_order_acq_rel)) {
            return;
        }

        const auto base =
            PRISMA_UI_API::RequestPluginAPI<
                PRISMA_UI_API::IVPrismaUI4>();
        const auto vr =
            PRISMA_UI_VR_API::RequestPluginVRAPI<
                PRISMA_UI_VR_API::IVPrismaUIVR1>();
        if (!base || !vr) {
            logger::error(
                "The PrismaUI base or FO4VR extension API is unavailable");
            return;
        }

        PRISMA_UI_VR_API::SpatialCapabilitiesV1
            capabilities{};
        capabilities.structSize =
            sizeof(capabilities);
        const auto result =
            vr->GetSpatialCapabilities(&capabilities);
        logger::info(
            "PrismaUI APIs resolved; spatial result={} features=0x{:X}",
            static_cast<std::int32_t>(result),
            capabilities.featureBits);
    }
}

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData version{};
    version.PluginVersion({1, 0, 0, 0});
    version.PluginName("PrismaUI-F4VR-Example");
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
    info->name = "PrismaUI-F4VR-Example";
    info->version =
        REL::Version(1, 0, 0, 0).pack();
    // F4SEVR 0.6.21 exposes its legacy 1.10.138 compatibility
    // value through QueryInterface::RuntimeVersion(). Validate
    // the actual executable instead.
    return !f4se->IsEditor() &&
           REL::Module::IsVR() &&
           REL::Module::get().version() ==
               F4SE::RUNTIME_VR_1_2_72;
}

F4SE_PLUGIN_LOAD(
    const F4SE::LoadInterface* f4se)
{
    if (!f4se) {
        return false;
    }
    F4SE::Init(f4se);
    const auto messaging =
        F4SE::GetMessagingInterface();
    return messaging &&
           messaging->RegisterListener(HandleMessage);
}
