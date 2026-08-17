#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/String.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

#include <DirectXTK/CommonStates.h>
#include <DirectXTK/SpriteBatch.h>
#include <DirectXTK/WICTextureLoader.h>
#include <d3d11.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Menus/FocusMenu/FocusMenu.h"
#include "Utils/NanoID.h"
#include "Utils/SingleThreadExecutor.h"

namespace PRISMA_UI_API {
enum class ConsoleMessageLevel : uint8_t;
}

namespace PrismaUI::Listeners {
class MyLoadListener;
class MyViewListener;
}

namespace PrismaUI::Core {
using namespace ultralight;

typedef uint64_t PrismaViewId;

struct PrismaView {
    PrismaViewId id = 0;
    RefPtr<View> ultralightView = nullptr;
    RefPtr<View> inspectorView = nullptr;
    std::string htmlPathToLoad;
    std::string originalUrl;
    std::string lastLoadedUrl;
    std::atomic<bool> isHidden{false};
    std::unique_ptr<Listeners::MyLoadListener> loadListener;
    std::unique_ptr<Listeners::MyViewListener> viewListener;
    std::atomic<bool> isLoadingFinished{false};
    std::function<void(const PrismaViewId&)> domReadyCallback;
    std::function<void(PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> consoleMessageCallback;
    std::string translationPluginName;
    std::unordered_map<std::string, std::string> translations;
    std::atomic<int> scrollingPixelSize{28};
    std::atomic<bool> isPaused{false};
    std::atomic<bool> nativePauseApplied{false};
    std::atomic<bool> isDestroying{false};
    int order = 0;
    std::atomic<bool> inspectorVisible{false};
    std::atomic<bool> needsRecovery{false};
    std::atomic<int> recoveryAttempts{0};
    std::atomic<uint32_t> faultCount{0};
    std::atomic<bool> quarantined{false};
    std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();

    std::vector<std::byte> inspectorPixelBuffer;
    uint32_t inspectorBufferWidth = 0;
    uint32_t inspectorBufferHeight = 0;
    uint32_t inspectorBufferStride = 0;
    std::mutex inspectorBufferMutex;
    std::atomic<bool> inspectorFrameReady{false};
    std::atomic<bool> inspectorPointerHover{false};
    ID3D11Texture2D* inspectorTexture = nullptr;
    ID3D11ShaderResourceView* inspectorTextureView = nullptr;
    uint32_t inspectorTextureWidth = 0;
    uint32_t inspectorTextureHeight = 0;
    std::atomic<float> inspectorPosX{0.0f};
    std::atomic<float> inspectorPosY{0.0f};
    std::atomic<uint32_t> inspectorDisplayWidth{0};
    std::atomic<uint32_t> inspectorDisplayHeight{0};
    std::atomic<float> inspectorOpacity{1.0f};

    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* textureView = nullptr;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    std::vector<std::byte> pixelBuffer;
    uint32_t bufferWidth = 0;
    uint32_t bufferHeight = 0;
    uint32_t bufferStride = 0;
    std::mutex bufferMutex;
    std::atomic<bool> newFrameReady{false};
    std::atomic<bool> pendingResourceRelease{false};

    std::mutex operationMutex;
    std::queue<std::function<void()>> pendingOperations;
    std::atomic<bool> isProcessingOperation{false};
    std::atomic<int> queuedOperationsCount{0};

    ~PrismaView();
};

extern SingleThreadExecutor ultralightThread;
extern NanoIdGenerator generator;
extern std::atomic<bool> coreInitialized;
extern std::atomic<bool> rendererInitFailed;
extern RefPtr<Renderer> renderer;
extern ID3D11Device* d3dDevice;
extern ID3D11DeviceContext* d3dContext;
extern HWND hWnd;

struct ScreenSize {
    std::atomic<std::uint32_t> width{0};
    std::atomic<std::uint32_t> height{0};
};
extern ScreenSize screenSize;

extern std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
extern std::unique_ptr<DirectX::CommonStates> commonStates;
extern Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorTexture;
extern std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
extern std::shared_mutex viewsMutex;

using SimpleJSCallback = std::function<void(std::string)>;

struct JSCallbackData {
    PrismaViewId viewId = 0;
    std::string name;
    SimpleJSCallback callback;
};

extern std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> jsCallbacks;
extern std::mutex jsCallbacksMutex;

void InitializeCoreSystem();
void InitHooks();
void InitGraphics();
void D3DPresent(uint32_t a_p1);
void OnResizeBuffers();
void Shutdown();
void CreateInspectorView(const PrismaViewId& viewId);
void SetInspectorVisibility(const PrismaViewId& viewId, bool visible);
bool IsInspectorVisible(const PrismaViewId& viewId);
void SetInspectorBounds(const PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width, uint32_t height);

}
