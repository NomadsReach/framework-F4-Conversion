#include "InputHandler.h"
#include "Core.h"
#include "ViewManager.h"
#include "Utils/Encoding.h"

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace PrismaUI::InputHandler {
	using namespace Core;

    HWND g_hWnd = nullptr;
    SingleThreadExecutor* g_ultralightThreadExecutor = nullptr;
    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* g_viewsMap = nullptr;
    std::shared_mutex* g_viewsMapMutex = nullptr;

    Core::PrismaViewId g_currentlyFocusedViewId;
    std::mutex g_focusedViewIdMutex;

    std::atomic<bool> g_isAnyInputCaptureActive = false;

    std::mutex g_eventQueueMutex;
    std::vector<InputEvent> g_eventQueue;

    const int SCROLL_LINES_PER_WHEEL_DELTA = 1;

    bool g_mouseButtonStates[3] = { false, false, false };

    // WndProc subclass state
    static std::atomic<bool> g_wndProcInstalled = false;
    static constexpr UINT_PTR SUBCLASS_ID = 0x505249534D41; // "PRISMA" in hex
    static std::mutex g_wndProcMutex;  // Thread-safe installation

    class MouseEventListener : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static MouseEventListener* GetSingleton() {
            static MouseEventListener singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, [[maybe_unused]] RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override {
            if (!a_event || !*a_event || !g_isAnyInputCaptureActive.load()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto cursor = RE::MenuCursor::GetSingleton();
            if (!cursor) {
                return RE::BSEventNotifyControl::kContinue;
            }

            for (auto event = *a_event; event; event = event->next) {
                switch (event->GetEventType()) {
                case RE::INPUT_EVENT_TYPE::kMouseMove: {
                    auto mouseMoveEvent = event->AsMouseMoveEvent();
                    if (mouseMoveEvent) {
                        ultralight::MouseEvent ev;
                        ev.type = ultralight::MouseEvent::kType_MouseMoved;
                        ev.x = static_cast<int>(cursor->cursorPosX);
                        ev.y = static_cast<int>(cursor->cursorPosY);
                        ev.button = ultralight::MouseEvent::kButton_None;

                        std::lock_guard lock(g_eventQueueMutex);
                        g_eventQueue.emplace_back(ev);
                    }
                    break;
                }

                case RE::INPUT_EVENT_TYPE::kButton: {
                    auto buttonEvent = event->AsButtonEvent();
                    if (!buttonEvent || buttonEvent->GetDevice() != RE::INPUT_DEVICE::kMouse)
                        break;

                    const auto idCode = buttonEvent->GetIDCode();
                    bool isPressed = buttonEvent->IsPressed();
                    bool isUp = buttonEvent->IsUp();

                    if (idCode <= 2) {
                        ultralight::MouseEvent::Button button = ultralight::MouseEvent::kButton_None;
                        switch (idCode) {
                        case 0: button = ultralight::MouseEvent::kButton_Left; break;
                        case 1: button = ultralight::MouseEvent::kButton_Right; break;
                        case 2: button = ultralight::MouseEvent::kButton_Middle; break;
                        }

                        if (isPressed && !g_mouseButtonStates[idCode]) {
                            g_mouseButtonStates[idCode] = true;

                            ultralight::MouseEvent ev;
                            ev.type = ultralight::MouseEvent::kType_MouseDown;
                            ev.x = static_cast<int>(cursor->cursorPosX);
                            ev.y = static_cast<int>(cursor->cursorPosY);
                            ev.button = button;

                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(ev);
                        }
                        else if (isUp && g_mouseButtonStates[idCode]) {
                            g_mouseButtonStates[idCode] = false;

                            ultralight::MouseEvent ev;
                            ev.type = ultralight::MouseEvent::kType_MouseUp;
                            ev.x = static_cast<int>(cursor->cursorPosX);
                            ev.y = static_cast<int>(cursor->cursorPosY);
                            ev.button = button;

                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(ev);
                        }
                    }

                    else if (idCode == 8 || idCode == 9) {
                        if (isPressed) {
                            ScrollEventWithPosition scrollWithPos;
                            scrollWithPos.event.type = ultralight::ScrollEvent::kType_ScrollByPixel;
                            scrollWithPos.event.delta_x = 0;
                            scrollWithPos.mouseX = static_cast<int>(cursor->cursorPosX);
                            scrollWithPos.mouseY = static_cast<int>(cursor->cursorPosY);

                    int scrollPixelSize = 28;

                            Core::PrismaViewId focusedViewId;
                            {
                                std::lock_guard lock(g_focusedViewIdMutex);
                                focusedViewId = g_currentlyFocusedViewId;
                            }

                            if (focusedViewId != 0) {
                                std::shared_lock lock(*g_viewsMapMutex);
                                auto it = g_viewsMap->find(focusedViewId);
                                if (it != g_viewsMap->end() && it->second) {
                                    scrollPixelSize = it->second->scrollingPixelSize;
                                }
                            }

                            int scrollAmount = SCROLL_LINES_PER_WHEEL_DELTA * scrollPixelSize;
                            if (idCode == 9) {
                                scrollWithPos.event.delta_y = -scrollAmount;
                            }
                            else {
                                scrollWithPos.event.delta_y = scrollAmount;
                            }

                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(scrollWithPos);
                        }
                    }
                    break;
                }

                default:
                    break;
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    LRESULT CALLBACK SubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
        if (g_isAnyInputCaptureActive.load()) {
            bool handledByUI = false;
            Core::PrismaViewId focusedViewIdCopy;
            {
                std::lock_guard lock(g_focusedViewIdMutex);
                focusedViewIdCopy = g_currentlyFocusedViewId;
            }

            if (focusedViewIdCopy != 0) {
                switch (uMsg) {
                case WM_KEYDOWN: {
                    ultralight::KeyEvent keyDownEvent = WinKeyHandler::CreateKeyEvent(
                        ultralight::KeyEvent::kType_RawKeyDown,
                        wParam,
                        lParam
                    );
                    {
                        std::lock_guard lock(g_eventQueueMutex);
                        g_eventQueue.emplace_back(keyDownEvent);
                    }
                    handledByUI = true;

                    BYTE kbdState[256];
                    GetKeyboardState(kbdState);
                    HKL currentLayout = GetKeyboardLayout(GetWindowThreadProcessId(hwnd, NULL));

                    wchar_t translatedChars[5] = { 0 };
                    int charCount = ToUnicodeEx(
                        (UINT)wParam,
                        ((lParam >> 16) & 0xFF),
                        kbdState,
                        translatedChars,
                        4,
                        0,
                        currentLayout
                    );

                    if (charCount > 0) {
                        bool viewHasInputFieldFocus = ViewManager::ViewHasInputFocus(focusedViewIdCopy);
                        if (viewHasInputFieldFocus) {
                            for (int i = 0; i < charCount; ++i) {
                                wchar_t ch = translatedChars[i];
                                if (ch >= 0x20 || ch == '\t') {
                                    ultralight::KeyEvent charEvent;
                                    charEvent.type = ultralight::KeyEvent::kType_Char;
                                    WinKeyHandler::GetUltralightModifiers(charEvent);

                                    wchar_t single_char_str[2] = { ch, 0 };
                                    char utf8_buffer[8] = { 0 };
                                    int utf8_len = WideCharToMultiByte(
                                        CP_UTF8, 0, single_char_str, -1,
                                        utf8_buffer, sizeof(utf8_buffer),
                                        nullptr, nullptr
                                    );
                                    std::string narrow_string = (utf8_len > 0) ?
                                        std::string(utf8_buffer) : std::string();
                                    ultralight::String ul_text = ultralight::Convert(narrow_string);

                                    charEvent.text = ul_text;
                                    charEvent.unmodified_text = ul_text;
                                    charEvent.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
                                    charEvent.key_identifier = "";
                                    charEvent.is_auto_repeat = (HIWORD(lParam) & KF_REPEAT) == KF_REPEAT;
                                    {
                                        std::lock_guard lock(g_eventQueueMutex);
                                        g_eventQueue.emplace_back(charEvent);
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
                case WM_KEYUP: {
                    ultralight::KeyEvent ev = WinKeyHandler::CreateKeyEvent(
                        ultralight::KeyEvent::kType_KeyUp,
                        wParam,
                        lParam
                    );
                    {
                        std::lock_guard lock(g_eventQueueMutex);
                        g_eventQueue.emplace_back(ev);
                    }
                    handledByUI = true;
                    break;
                }
                case WM_CHAR: {
                    bool viewHasInputFieldFocus = ViewManager::ViewHasInputFocus(focusedViewIdCopy);
                    if (viewHasInputFieldFocus) {
                        handledByUI = true;
                    }
                    break;
                }
                default:
                    break;
                }
            }

            if (handledByUI) {
                return 0;
            }
        }

        // Handle window destruction - clean up subclass
        if (uMsg == WM_NCDESTROY) {
            RemoveWindowSubclass(hwnd, SubclassProc, uIdSubclass);
            if (uIdSubclass == SUBCLASS_ID) {
                g_wndProcInstalled.store(false);
            }
        }

        // Pass to next handler in the chain using DefSubclassProc
        // This properly handles the chain even if other mods are in the stack
        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }

    void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor, std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap, std::shared_mutex* viewsMapMutex) {
        g_hWnd = gameHwnd;
        g_ultralightThreadExecutor = coreExecutor;
        g_viewsMap = viewsMap;
        g_viewsMapMutex = viewsMapMutex;
        g_isAnyInputCaptureActive = false;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            g_currentlyFocusedViewId = 0;
        }

        g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;

        logger::info("PrismaUI::InputHandler Initialized with HWND: {}", (void*)g_hWnd);

        auto inputEventSource = RE::BSInputDeviceManager::GetSingleton();
        if (inputEventSource) {
            inputEventSource->AddEventSink(MouseEventListener::GetSingleton());
            logger::info("MouseEventListener registered with BSInputDeviceManager");
        }
        else {
            logger::error("Failed to register MouseEventListener: BSInputDeviceManager is null");
        }
    }

    bool InstallWndProcHook() {
        std::lock_guard<std::mutex> lock(g_wndProcMutex);

        if (g_wndProcInstalled.load()) {
            logger::debug("WndProc subclass already installed");
            return true;
        }

        if (!g_hWnd) {
            logger::error("Cannot install WndProc subclass: HWND is null");
            return false;
        }

        // Check if HWND is valid
        if (!IsWindow(g_hWnd)) {
            logger::error("HWND {:p} is not a valid window", (void*)g_hWnd);
            return false;
        }

        logger::debug("Attempting to install subclass on HWND: {:p}", (void*)g_hWnd);

        // Clear last error before calling
        SetLastError(0);

        if (!SetWindowSubclass(g_hWnd, SubclassProc, SUBCLASS_ID, 0)) {
            DWORD err = GetLastError();
            logger::error("Failed to install WndProc subclass. Error: {} (0x{:X})", err, err);
            return false;
        }

        g_wndProcInstalled.store(true);
        logger::info("WndProc subclass installed successfully on HWND: {:p}", (void*)g_hWnd);
        return true;
    }

    void UninstallWndProcHook() {
        std::lock_guard<std::mutex> lock(g_wndProcMutex);

        if (!g_wndProcInstalled.exchange(false)) {
            return;
        }

        if (g_hWnd) {
            RemoveWindowSubclass(g_hWnd, SubclassProc, SUBCLASS_ID);
            logger::info("WndProc subclass removed");
        }
    }

    void EnableInputCapture(const Core::PrismaViewId& viewId) {
        if (viewId == 0) {
            logger::warn("EnableInputCapture called with empty viewId.");
            return;
        }
        bool firstTimeActivation = false;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            if (g_currentlyFocusedViewId != viewId) {
                g_currentlyFocusedViewId = viewId;
                logger::debug("PrismaUI Input Capture focused on View [{}].", viewId);
            }
        }

        if (!g_isAnyInputCaptureActive.exchange(true)) {
            firstTimeActivation = true;
            logger::debug("PrismaUI Input Capture System Enabled for View [{}].", viewId);
        }

        g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;
    }

    void DisableInputCapture(const Core::PrismaViewId& viewIdToUnfocus) {
        bool disableSystem = false;
        Core::PrismaViewId currentFocusedBeforeDisable;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            currentFocusedBeforeDisable = g_currentlyFocusedViewId;
            if (viewIdToUnfocus == 0 || viewIdToUnfocus == g_currentlyFocusedViewId) {
                if (g_isAnyInputCaptureActive.load()) {
                    disableSystem = true;
                    g_currentlyFocusedViewId = 0;
                }
            }
        }

        if (disableSystem) {
            if (g_isAnyInputCaptureActive.exchange(false)) {
                logger::debug("PrismaUI Input Capture System Disabled (was active for View [{}]).", currentFocusedBeforeDisable);

                g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;

                if (g_ultralightThreadExecutor && currentFocusedBeforeDisable != 0) {
                    g_ultralightThreadExecutor->submit([viewId_copy = currentFocusedBeforeDisable]() {
                        std::shared_ptr<Core::PrismaView> targetViewData = nullptr;
                        {
                            std::shared_lock lock(*g_viewsMapMutex);
                            auto it = g_viewsMap->find(viewId_copy);
                            if (it != g_viewsMap->end()) {
                                targetViewData = it->second;
                            }
                        }

                        if (targetViewData && targetViewData->ultralightView) {
                            logger::debug("Resetting mouse position to (0,0) for View [{}]", viewId_copy);
                            ultralight::MouseEvent resetEvent;
                            resetEvent.type = ultralight::MouseEvent::kType_MouseMoved;
                            resetEvent.x = 0;
                            resetEvent.y = 0;
                            resetEvent.button = ultralight::MouseEvent::kButton_None;

                            targetViewData->ultralightView->FireMouseEvent(resetEvent);
                        }
                        });
                }
            }
        }
        else if (viewIdToUnfocus != 0) {
            logger::debug("PrismaUI: DisableInputCapture called for View [{}] but View [{}] is/was focused. No change to system state, only unfocused ID removed if it matched.", viewIdToUnfocus, currentFocusedBeforeDisable);
        }
    }

    bool IsAnyInputCaptureActive() {
        return g_isAnyInputCaptureActive.load();
    }

    bool IsInputCaptureActiveForView(const Core::PrismaViewId& viewId) {
        Core::PrismaViewId currentFocused;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            currentFocused = g_currentlyFocusedViewId;
        }
        if (viewId == 0) return g_isAnyInputCaptureActive.load();
        return g_isAnyInputCaptureActive.load() && (currentFocused == viewId);
    }

    void ProcessEvents() {
        if (!g_ultralightThreadExecutor || !g_viewsMap || !g_viewsMapMutex) return;

        Core::PrismaViewId focusedViewIdCopy;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            focusedViewIdCopy = g_currentlyFocusedViewId;
        }

        if (focusedViewIdCopy == 0 && !g_eventQueue.empty()) {
            std::lock_guard lock(g_eventQueueMutex);
            g_eventQueue.clear();
            return;
        }
        if (focusedViewIdCopy == 0) return;

        std::vector<InputEvent> eventsToProcess;
        {
            std::lock_guard lock(g_eventQueueMutex);
            if (g_eventQueue.empty()) return;
            eventsToProcess.swap(g_eventQueue);
        }

        g_ultralightThreadExecutor->submit([viewId_copy = focusedViewIdCopy, ev_queue = std::move(eventsToProcess)]() {
            std::shared_ptr<Core::PrismaView> targetViewData = nullptr;
            {
                std::shared_lock lock(*g_viewsMapMutex);
                auto it = g_viewsMap->find(viewId_copy);
                if (it != g_viewsMap->end()) {
                    targetViewData = it->second;
                }
            }

            if (targetViewData && targetViewData->ultralightView) {
                ultralight::View* ulView = targetViewData->ultralightView.get();
                ultralight::View* inspectorView = targetViewData->inspectorView ? targetViewData->inspectorView.get() : nullptr;
                
                for (const auto& event_variant : ev_queue) {
                    std::visit([ulView, inspectorView, &targetViewData](const auto& arg) {
                        using T = std::decay_t<decltype(arg)>;
                        if constexpr (std::is_same_v<T, ultralight::MouseEvent>) {
                            // Check if mouse is over inspector bounds when inspector is visible
                            bool mouseOverInspector = false;
                            if (inspectorView && targetViewData->inspectorVisible.load()) {
                                const float inspX = targetViewData->inspectorPosX;
                                const float inspY = targetViewData->inspectorPosY;
                                const float inspW = static_cast<float>(targetViewData->inspectorDisplayWidth);
                                const float inspH = static_cast<float>(targetViewData->inspectorDisplayHeight);
                                
                                const float mouseX = static_cast<float>(arg.x);
                                const float mouseY = static_cast<float>(arg.y);
                                
                                if (mouseX >= inspX && mouseX < (inspX + inspW) &&
                                    mouseY >= inspY && mouseY < (inspY + inspH)) {
                                    mouseOverInspector = true;
                                    targetViewData->inspectorPointerHover.store(true);
                                } else {
                                    targetViewData->inspectorPointerHover.store(false);
                                }
                            }
                            
                            if (mouseOverInspector) {
                                // Translate mouse coordinates to inspector view
                                ultralight::MouseEvent inspectorEvent = arg;
                                inspectorEvent.x = arg.x - static_cast<int>(targetViewData->inspectorPosX);
                                inspectorEvent.y = arg.y - static_cast<int>(targetViewData->inspectorPosY);
                                inspectorView->FireMouseEvent(inspectorEvent);
                            } else {
                                ulView->FireMouseEvent(arg);
                            }
                        } else if constexpr (std::is_same_v<T, ScrollEventWithPosition>) {
                            // Route scroll events to inspector if mouse is over it
                            // Use captured mouse position for accurate bounds checking
                            bool scrollOverInspector = false;
                            if (inspectorView && targetViewData->inspectorVisible.load()) {
                                const float inspX = targetViewData->inspectorPosX;
                                const float inspY = targetViewData->inspectorPosY;
                                const float inspW = static_cast<float>(targetViewData->inspectorDisplayWidth);
                                const float inspH = static_cast<float>(targetViewData->inspectorDisplayHeight);
                                
                                const float mouseX = static_cast<float>(arg.mouseX);
                                const float mouseY = static_cast<float>(arg.mouseY);
                                
                                if (mouseX >= inspX && mouseX < (inspX + inspW) &&
                                    mouseY >= inspY && mouseY < (inspY + inspH)) {
                                    scrollOverInspector = true;
                                }
                            }
                            if (scrollOverInspector) {
                                inspectorView->FireScrollEvent(arg.event);
                            } else {
                                ulView->FireScrollEvent(arg.event);
                            }
                        } else if constexpr (std::is_same_v<T, ultralight::KeyEvent>) {
                            // Route keyboard events to inspector if it's visible and focused
                            if (inspectorView && targetViewData->inspectorVisible.load() && 
                                inspectorView->HasFocus()) {
                                inspectorView->FireKeyEvent(arg);
                            } else {
                                ulView->FireKeyEvent(arg);
                            }
                        }
                    }, event_variant);
                }
            }
        });
    }

    void Shutdown() {
        DisableInputCapture(0);
        { std::lock_guard lock(g_eventQueueMutex); g_eventQueue.clear(); }

        auto inputEventSource = RE::BSInputDeviceManager::GetSingleton();
        if (inputEventSource) {
            inputEventSource->RemoveEventSink(MouseEventListener::GetSingleton());
            logger::debug("MouseEventListener removed from BSInputDeviceManager");
        }

        UninstallWndProcHook();

        g_hWnd = nullptr;
        g_ultralightThreadExecutor = nullptr;
        g_viewsMap = nullptr;
        g_viewsMapMutex = nullptr;
        logger::info("PrismaUI::InputHandler Shutdown.");
    }
}
