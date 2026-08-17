#pragma once

#include "Utils/WinKeyHandler/WinKeyHandler.h"

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <variant>

class SingleThreadExecutor;

namespace PrismaUI::Core {
typedef uint64_t PrismaViewId;
struct PrismaView;
}

namespace PrismaUI::InputHandler {
using namespace ultralight;

struct ScrollEventWithPosition {
    ScrollEvent event;
    int mouseX;
    int mouseY;
};

using InputEvent = std::variant<MouseEvent, ScrollEventWithPosition, KeyEvent>;
using OverlayClickCallback = std::function<bool(int x, int y)>;

void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor,
                std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                std::shared_mutex* viewsMapMutex);
void EnableInputCapture(const Core::PrismaViewId& viewId);
void DisableInputCapture(const Core::PrismaViewId& viewId);
void ClearImeState(const Core::PrismaViewId& viewId);
bool IsInputCaptureActiveForView(const Core::PrismaViewId& viewId);
bool IsAnyInputCaptureActive();
Core::PrismaViewId GetFocusedViewId();
int GetLastCursorX();
int GetLastCursorY();
bool InstallWndProcHook();
void UninstallWndProcHook();
void ProcessEvents();
void Shutdown();
void RegisterOverlayClickHandler(OverlayClickCallback cb);

}
