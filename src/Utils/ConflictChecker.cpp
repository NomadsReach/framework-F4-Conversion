#include "ConflictChecker.h"

#include <Psapi.h>
#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace PrismaUI::ConflictChecker {
namespace {

std::wstring GetModuleBasename(HMODULE module)
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"<unknown>";
    std::wstring full(path, length);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            output.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return output;
}

std::string WideToUtf8(const wchar_t* value)
{
    return value ? WideToUtf8(std::wstring(value)) : std::string{};
}

std::string OwnerOf(void* address)
{
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &owner) && owner) {
        return WideToUtf8(GetModuleBasename(owner));
    }
    return "<unknown>";
}

std::vector<HMODULE> LoadedModules()
{
    HANDLE process = GetCurrentProcess();
    std::vector<HMODULE> modules(256);

    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD requiredBytes = 0;
        if (!EnumProcessModules(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                &requiredBytes)) {
            logger::warn("[ConflictChecker] EnumProcessModules failed (GLE={})", GetLastError());
            return {};
        }

        const size_t requiredCount = requiredBytes / sizeof(HMODULE);
        if (requiredCount <= modules.size()) {
            modules.resize(requiredCount);
            return modules;
        }
        modules.resize(requiredCount + 32);
    }

    logger::warn("[ConflictChecker] module list changed repeatedly during enumeration");
    return {};
}

struct APIConsumer {
    std::string name;
    PRISMA_UI_API::InterfaceVersion version;
};

std::vector<APIConsumer> g_apiConsumers;
std::mutex g_apiConsumersMutex;

void PrintAPIConsumerSummary()
{
    std::lock_guard lock(g_apiConsumersMutex);
    if (g_apiConsumers.empty()) return;

    std::string summary;
    for (size_t i = 0; i < g_apiConsumers.size(); ++i) {
        if (i) summary += ", ";
        summary += g_apiConsumers[i].name;
        summary += " (V" + std::to_string(static_cast<int>(g_apiConsumers[i].version)) + ")";
    }
    logger::info("[ConflictChecker] API consumers: {}", summary);
}

constexpr const wchar_t* kKnownConflictDlls[] = {
    L"ReShade.dll",
    L"ReShade64.dll",
    L"FallSouls.dll",
    L"FallSouls_NG.dll",
    L"enbseries.dll",
    L"d3d11_log.dll",
};

}

void CheckEarly()
{
    const auto modules = LoadedModules();
    std::vector<std::wstring> matches;
    for (HMODULE module : modules) {
        std::wstring basename = GetModuleBasename(module);
        std::wstring lower = basename;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return std::towlower(c); });
        if (lower != L"prismaui_f4.dll") continue;

        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        matches.emplace_back(path, length > 0 && length < MAX_PATH ? length : 0);
    }

    if (matches.size() > 1) {
        logger::critical("[ConflictChecker] PrismaUI_F4.dll loaded {} times", matches.size());
        for (const auto& path : matches) logger::critical("[ConflictChecker] {}", WideToUtf8(path));
    }

    HMODULE ours = GetModuleHandleW(L"PrismaUI_F4.dll");
    if (!ours) return;

    void* exported = reinterpret_cast<void*>(GetProcAddress(ours, "RequestPluginAPI"));
    if (!exported) return;

    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(exported), &owner) && owner && owner != ours) {
        logger::critical("[ConflictChecker] RequestPluginAPI resolves to '{}'", WideToUtf8(GetModuleBasename(owner)));
    }
}

void CheckPreHooks()
{
    for (const auto* name : kKnownConflictDlls) {
        if (GetModuleHandleW(name)) logger::warn("[ConflictChecker] loaded D3D wrapper: {}", WideToUtf8(name));
    }

    auto* rendererData = RE::BSGraphics::GetRendererData();
    if (rendererData && rendererData->renderWindow[0].swapChain) {
        auto* swapChain = rendererData->renderWindow[0].swapChain;
        void** vtable = *reinterpret_cast<void***>(swapChain);
        if (vtable && vtable[8] && vtable[13]) {
            uintptr_t dxgiBase = 0;
            uintptr_t dxgiEnd = 0;
            if (HMODULE dxgi = GetModuleHandleW(L"dxgi.dll")) {
                MODULEINFO info{};
                if (GetModuleInformation(GetCurrentProcess(), dxgi, &info, sizeof(info))) {
                    dxgiBase = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
                    dxgiEnd = dxgiBase + info.SizeOfImage;
                }
            }

            auto check = [&](int index, const char* label) {
                const uintptr_t address = reinterpret_cast<uintptr_t>(vtable[index]);
                const bool inDxgi = dxgiBase != 0 && address >= dxgiBase && address < dxgiEnd;
                if (!inDxgi) {
                    logger::warn("[ConflictChecker] {} is wrapped by '{}' at 0x{:016X}", label,
                                 OwnerOf(vtable[index]), address);
                }
            };
            check(8, "Present");
            check(13, "ResizeBuffers");
        }
    }

    PrintAPIConsumerSummary();
}

void OnAPIRequest(void* returnAddress, PRISMA_UI_API::InterfaceVersion version)
{
    std::string caller = "<unknown>";
    HMODULE module = nullptr;
    if (returnAddress &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(returnAddress), &module) && module) {
        caller = WideToUtf8(GetModuleBasename(module));
    }

    const bool supported = version == PRISMA_UI_API::InterfaceVersion::V1 ||
                           version == PRISMA_UI_API::InterfaceVersion::V2 ||
                           version == PRISMA_UI_API::InterfaceVersion::V3 ||
                           version == PRISMA_UI_API::InterfaceVersion::V4;
    if (!supported) {
        logger::error("[ConflictChecker] '{}' requested unsupported interface V{}", caller,
                      static_cast<int>(version));
    }

    std::lock_guard lock(g_apiConsumersMutex);
    auto it = std::find_if(g_apiConsumers.begin(), g_apiConsumers.end(), [&](const APIConsumer& consumer) {
        return consumer.name == caller && consumer.version == version;
    });
    if (it == g_apiConsumers.end()) g_apiConsumers.push_back({std::move(caller), version});
}

}
