#include "InputHandler.h"

#include <algorithm>
#include <commctrl.h>
#include <cstddef>
#include <cstdio>
#include <type_traits>
#include <vector>
#include <windowsx.h>

#include "Communication.h"
#include "Core.h"
#include "ImeHelper.h"
#include "Utils/Encoding.h"

#pragma comment(lib, "comctl32.lib")

namespace PrismaUI::InputHandler {
    using namespace Core;

    namespace {
        using MouseButton = decltype(ultralight::MouseEvent::kButton_Left);

        struct QueuedInputEvent {
            Core::PrismaViewId viewId = 0;
            InputEvent event;
        };
    }

    HWND g_hWnd = nullptr;
    SingleThreadExecutor* g_ultralightThreadExecutor = nullptr;
    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* g_viewsMap = nullptr;
    std::shared_mutex* g_viewsMapMutex = nullptr;

    Core::PrismaViewId g_currentlyFocusedViewId = 0;
    std::mutex g_focusedViewIdMutex;
    std::atomic<bool> g_isAnyInputCaptureActive = false;

    std::mutex g_eventQueueMutex;
    std::vector<QueuedInputEvent> g_eventQueue;

    bool g_pointerDragOwnedByUI = false;
    bool g_mouseButtonStates[3] = {false, false, false};
    wchar_t g_pendingHighSurrogate = 0;

    static std::atomic<int> g_lastCursorX = 0;
    static std::atomic<int> g_lastCursorY = 0;

    static std::atomic<bool> g_wndProcInstalled = false;
    static constexpr UINT_PTR kSubclassId = 0x505249534D41;
    static std::mutex g_wndProcMutex;

    static std::mutex g_overlayClickMutex;
    static std::vector<OverlayClickCallback> g_overlayClickHandlers;

    static ImeHelper g_imeHelper;
    static std::atomic<bool> g_isFocusedTextInputActive = false;

    constexpr int kScrollLinesPerWheelDelta = 1;
    constexpr size_t kMaxClipboardSize = 1024 * 1024;
    constexpr size_t kMaxClipboardChars = 200000;
    constexpr const char* kImeFocusCallbackName = "__prismaNativeImeFocusChanged";

    static Core::PrismaViewId GetFocusedViewIdSnapshot() {
        std::lock_guard lock(g_focusedViewIdMutex);
        return g_currentlyFocusedViewId;
    }

    static std::shared_ptr<Core::PrismaView> FindLiveView(Core::PrismaViewId viewId) {
        if (viewId == 0 || !g_viewsMap || !g_viewsMapMutex) {
            return nullptr;
        }

        std::shared_lock lock(*g_viewsMapMutex);
        auto it = g_viewsMap->find(viewId);
        if (it == g_viewsMap->end() || !it->second || it->second->isDestroying.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return it->second;
    }

    static void QueueInputForView(Core::PrismaViewId viewId, InputEvent event) {
        if (viewId == 0) {
            return;
        }
        std::lock_guard lock(g_eventQueueMutex);
        g_eventQueue.push_back({viewId, std::move(event)});
    }

    static void DiscardQueuedEventsForView(Core::PrismaViewId viewId) {
        std::lock_guard lock(g_eventQueueMutex);
        if (viewId == 0) {
            g_eventQueue.clear();
            return;
        }
        std::erase_if(g_eventQueue, [viewId](const QueuedInputEvent& queued) { return queued.viewId == viewId; });
    }

    static void DiscardQueuedEventsExcept(Core::PrismaViewId viewId) {
        std::lock_guard lock(g_eventQueueMutex);
        std::erase_if(g_eventQueue, [viewId](const QueuedInputEvent& queued) { return queued.viewId != viewId; });
    }

    static bool AnyMouseButtonDown() {
        return g_mouseButtonStates[0] || g_mouseButtonStates[1] || g_mouseButtonStates[2];
    }

    // Transparent pixels intentionally pass clicks through to the game world behind the view.
    static bool IsCursorOverContent(Core::PrismaViewId viewId, int x, int y) {
        auto viewData = FindLiveView(viewId);
        if (!viewData) {
            return false;
        }

        std::lock_guard lock(viewData->bufferMutex);
        if (viewData->isDestroying.load(std::memory_order_acquire)) {
            return false;
        }
        if (viewData->pixelBuffer.empty() || viewData->bufferWidth == 0 || viewData->bufferHeight == 0 ||
            viewData->bufferStride == 0) {
            return true;
        }
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= viewData->bufferWidth ||
            static_cast<uint32_t>(y) >= viewData->bufferHeight) {
            return false;
        }

        const size_t offset =
            static_cast<size_t>(y) * viewData->bufferStride + static_cast<size_t>(x) * 4u + 3u;
        if (offset >= viewData->pixelBuffer.size()) {
            return true;
        }
        return std::to_integer<std::uint8_t>(viewData->pixelBuffer[offset]) >= 12;
    }

    static bool HandleMouseButtonDown(Core::PrismaViewId viewId, size_t stateIndex, MouseButton button, int x,
                                      int y) {
        const bool wasAnyButtonDown = AnyMouseButtonDown();
        g_mouseButtonStates[stateIndex] = true;
        if (!wasAnyButtonDown) {
            g_pointerDragOwnedByUI = IsCursorOverContent(viewId, x, y);
        }
        if (!g_pointerDragOwnedByUI) {
            return false;
        }

        ultralight::MouseEvent event;
        event.type = ultralight::MouseEvent::kType_MouseDown;
        event.x = x;
        event.y = y;
        event.button = button;
        QueueInputForView(viewId, event);
        return true;
    }

    static bool HandleMouseButtonUp(Core::PrismaViewId viewId, size_t stateIndex, MouseButton button, int x, int y) {
        g_mouseButtonStates[stateIndex] = false;
        const bool capture = g_pointerDragOwnedByUI;
        if (capture) {
            ultralight::MouseEvent event;
            event.type = ultralight::MouseEvent::kType_MouseUp;
            event.x = x;
            event.y = y;
            event.button = button;
            QueueInputForView(viewId, event);
        }
        if (!AnyMouseButtonDown()) {
            g_pointerDragOwnedByUI = false;
        }
        return capture;
    }

    std::string BuildImeFocusTrackingScript() {
        const std::string callbackName = kImeFocusCallbackName;
        return "(function(){"
               "if(window.__prismaImeFocusTrackingInstalled){"
               "if(typeof window.__prismaImeFocusNotify==='function'){window.__prismaImeFocusNotify(document.activeElement);}"
               "return;"
               "}"
               "function isTextInputElement(el){"
               "if(!el||el.disabled||el.readOnly)return false;"
               "if(el.isContentEditable)return true;"
               "var tag=(el.tagName||'').toUpperCase();"
               "if(tag==='TEXTAREA')return true;"
               "if(tag!=='INPUT')return false;"
               "var type=((el.type||'text')+'').toLowerCase();"
               "switch(type){"
               "case '':case 'text':case 'search':case 'url':case 'tel':case 'password':case 'email':case 'number':"
               "return true;"
               "default:return false;"
               "}"
               "}"
               "function notify(element){"
               "var focused=isTextInputElement(element)?'1':'0';"
               "if(typeof window['" + callbackName + "']==='function'){window['" + callbackName + "'](focused);}"
               "}"
               "window.__prismaImeFocusNotify=notify;"
               "window.__prismaImeFocusTrackingInstalled=true;"
               "document.addEventListener('focusin',function(event){notify(event.target);},true);"
               "document.addEventListener('focusout',function(){setTimeout(function(){notify(document.activeElement);},0);},true);"
               "notify(document.activeElement);"
               "})();";
    }

    void InstallImeFocusTrackingForView(const Core::PrismaViewId& viewId) {
        if (!g_ultralightThreadExecutor || !g_viewsMap || !g_viewsMapMutex || viewId == 0) {
            return;
        }

        auto* viewsMap = g_viewsMap;
        auto* viewsMapMutex = g_viewsMapMutex;
        auto install = [viewId, viewsMap, viewsMapMutex]() {
            std::shared_ptr<Core::PrismaView> viewData;
            {
                std::shared_lock lock(*viewsMapMutex);
                auto it = viewsMap->find(viewId);
                if (it != viewsMap->end()) {
                    viewData = it->second;
                }
            }

            if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView ||
                !viewData->isLoadingFinished.load()) {
                return;
            }

            Communication::BindJSCallbacks(viewId);
            if (viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) {
                return;
            }

            try {
                ultralight::String script = BuildImeFocusTrackingScript().c_str();
                viewData->ultralightView->EvaluateScript(script, nullptr);
            } catch (const std::exception& e) {
                logger::error("Failed to install IME focus tracking for View [{}]: {}", viewId, e.what());
            } catch (...) {
                logger::error("Failed to install IME focus tracking for View [{}]: unknown exception", viewId);
            }
        };

        if (g_ultralightThreadExecutor->IsWorkerThread()) {
            install();
        } else {
            g_ultralightThreadExecutor->submit(std::move(install));
        }
    }

    std::string EscapeForJS(const std::string& text) {
        std::string escaped;
        try {
            escaped.reserve(text.size() * 2);
        } catch (const std::exception& e) {
            logger::error("Failed to allocate memory for escaped text: {}", e.what());
            return "";
        }

        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char value = static_cast<unsigned char>(text[i]);
            if (value >= 0x80) {
                if (i + 2 < text.size() && value == 0xE2 &&
                    static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                    (static_cast<unsigned char>(text[i + 2]) == 0xA8 ||
                     static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
                    escaped += "\\u202";
                    escaped += static_cast<unsigned char>(text[i + 2]) == 0xA8 ? '8' : '9';
                    i += 2;
                    continue;
                }
                escaped += static_cast<char>(value);
                continue;
            }

            switch (value) {
                case '\'':
                    escaped += "\\'";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                case '\b':
                    escaped += "\\b";
                    break;
                case '\f':
                    escaped += "\\f";
                    break;
                default:
                    if (value < 0x20) {
                        char buffer[7];
                        snprintf(buffer, sizeof(buffer), "\\u%04X", value);
                        escaped += buffer;
                    } else {
                        escaped += static_cast<char>(value);
                    }
                    break;
            }
        }
        return escaped;
    }

    std::string GetClipboardText() {
        if (!OpenClipboard(g_hWnd)) {
            logger::warn("Failed to open clipboard for reading");
            return "";
        }

        HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
        if (!clipboardData) {
            CloseClipboard();
            return "";
        }

        auto* text = static_cast<wchar_t*>(GlobalLock(clipboardData));
        if (!text) {
            CloseClipboard();
            return "";
        }

        const SIZE_T dataSize = GlobalSize(clipboardData);
        if (dataSize == 0 || dataSize > kMaxClipboardSize) {
            logger::warn("Clipboard text size is invalid or too large: {} bytes (max: {} bytes)", dataSize,
                         kMaxClipboardSize);
            GlobalUnlock(clipboardData);
            CloseClipboard();
            return "";
        }

        const size_t maxChars = dataSize / sizeof(wchar_t);
        size_t charCount = 0;
        while (charCount < maxChars && text[charCount] != L'\0') {
            ++charCount;
        }
        if (charCount == maxChars || charCount > kMaxClipboardChars) {
            logger::warn("Clipboard text is not terminated or exceeds {} characters", kMaxClipboardChars);
            GlobalUnlock(clipboardData);
            CloseClipboard();
            return "";
        }

        std::string result;
        if (charCount > 0) {
            const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(charCount), nullptr, 0,
                                                       nullptr, nullptr);
            if (utf8Length <= 0) {
                GlobalUnlock(clipboardData);
                CloseClipboard();
                return "";
            }

            try {
                result.resize(static_cast<size_t>(utf8Length));
                WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(charCount), result.data(), utf8Length, nullptr,
                                    nullptr);
            } catch (const std::exception& e) {
                logger::error("Failed to allocate memory for clipboard text: {}", e.what());
                GlobalUnlock(clipboardData);
                CloseClipboard();
                return "";
            }
        }

        GlobalUnlock(clipboardData);
        CloseClipboard();
        return result;
    }

    void SetClipboardText(const std::string& text) {
        if (text.size() > kMaxClipboardSize) {
            logger::warn("Text too large to copy to clipboard: {} bytes (max: {} bytes)", text.size(),
                         kMaxClipboardSize);
            return;
        }
        if (!OpenClipboard(g_hWnd)) {
            logger::warn("Failed to open clipboard for writing");
            return;
        }

        EmptyClipboard();
        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wideLength <= 0) {
            CloseClipboard();
            return;
        }

        HGLOBAL memory = nullptr;
        try {
            memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wideLength) * sizeof(wchar_t));
            if (!memory) {
                logger::error("Failed to allocate global memory for clipboard");
                CloseClipboard();
                return;
            }

            auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
            if (!destination) {
                GlobalFree(memory);
                CloseClipboard();
                return;
            }

            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, destination, wideLength);
            GlobalUnlock(memory);

            if (!SetClipboardData(CF_UNICODETEXT, memory)) {
                GlobalFree(memory);
                logger::warn("Failed to set clipboard data");
            } else {
                memory = nullptr;
            }
        } catch (const std::exception& e) {
            logger::error("Exception while setting clipboard text: {}", e.what());
            if (memory) {
                GlobalFree(memory);
            }
        }

        CloseClipboard();
    }

    bool IsHighSurrogate(wchar_t ch) { return ch >= 0xD800 && ch <= 0xDBFF; }
    bool IsLowSurrogate(wchar_t ch) { return ch >= 0xDC00 && ch <= 0xDFFF; }
    bool ShouldQueueChar(wchar_t ch) { return ch >= 0x20 || ch == '\t'; }

    std::string ConvertUtf16ToUtf8(const wchar_t* text, int length) {
        if (!text || length <= 0) {
            return "";
        }

        const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) {
            return "";
        }

        std::string result;
        try {
            result.resize(static_cast<size_t>(utf8Length));
            WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), utf8Length, nullptr, nullptr);
        } catch (const std::exception& e) {
            logger::error("Failed to allocate memory for committed text: {}", e.what());
            return "";
        }
        return result;
    }

    void QueueCommittedCharEvent(const std::wstring& utf16Text, LPARAM lParam) {
        if (utf16Text.empty()) {
            return;
        }

        const Core::PrismaViewId viewId = GetFocusedViewIdSnapshot();
        if (viewId == 0) {
            return;
        }

        std::string utf8Text = ConvertUtf16ToUtf8(utf16Text.data(), static_cast<int>(utf16Text.size()));
        if (utf8Text.empty()) {
            logger::warn("Failed to convert committed text to UTF-8");
            return;
        }

        ultralight::KeyEvent charEvent;
        charEvent.type = ultralight::KeyEvent::kType_Char;
        WinKeyHandler::GetUltralightModifiers(charEvent);
        ultralight::String ulText(utf8Text.c_str());
        charEvent.text = ulText;
        charEvent.unmodified_text = ulText;
        charEvent.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
        charEvent.native_key_code = 0;
        charEvent.key_identifier = "";
        charEvent.is_keypad = false;
        charEvent.is_auto_repeat = (HIWORD(lParam) & KF_REPEAT) == KF_REPEAT;
        charEvent.is_system_key = false;
        QueueInputForView(viewId, charEvent);
    }

    std::wstring ConvertCodePointToUtf16(UINT codePoint) {
        if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return L"";
        }
        if (codePoint <= 0xFFFF) {
            return std::wstring(1, static_cast<wchar_t>(codePoint));
        }

        codePoint -= 0x10000;
        wchar_t high = static_cast<wchar_t>(0xD800 + (codePoint >> 10));
        wchar_t low = static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF));
        return std::wstring{high, low};
    }

    LRESULT CALLBACK SubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId,
                                  DWORD_PTR /*refData*/) {
        LRESULT imeControlResult = 0;
        if (g_imeHelper.HandleControlMessage(hwnd, message, wParam, lParam, &imeControlResult)) {
            return imeControlResult;
        }

        if (message == WM_IME_SETCONTEXT) {
            g_imeHelper.ModifySetContextLParam(&lParam, message);
        }

        if (message == WM_MOUSEMOVE) {
            g_lastCursorX.store(GET_X_LPARAM(lParam));
            g_lastCursorY.store(GET_Y_LPARAM(lParam));
        }

        if (message == WM_LBUTTONDOWN) {
            const int clickX = GET_X_LPARAM(lParam);
            const int clickY = GET_Y_LPARAM(lParam);
            std::vector<OverlayClickCallback> handlers;
            {
                std::lock_guard lock(g_overlayClickMutex);
                handlers = g_overlayClickHandlers;
            }
            for (const auto& handler : handlers) {
                if (!handler) {
                    continue;
                }
                try {
                    if (handler(clickX, clickY)) {
                        return 0;
                    }
                } catch (const std::exception& e) {
                    logger::error("Overlay click handler threw an exception: {}", e.what());
                } catch (...) {
                    logger::error("Overlay click handler threw an unknown exception");
                }
            }
        }

        if (g_isAnyInputCaptureActive.load()) {
            if (message == WM_SETCURSOR) {
                SetCursor(nullptr);
                return TRUE;
            }

            bool handledByUI = false;
            const Core::PrismaViewId focusedViewId = GetFocusedViewIdSnapshot();
            if (focusedViewId != 0 && FindLiveView(focusedViewId)) {
                if (g_imeHelper.HandleMessage(hwnd, message, wParam, lParam, focusedViewId, &handledByUI) &&
                    handledByUI) {
                    return 0;
                }

                switch (message) {
                    case WM_KEYDOWN: {
                        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'V') {
                            if (g_isFocusedTextInputActive.load()) {
                                try {
                                    std::string clipboardText = GetClipboardText();
                                    if (!clipboardText.empty()) {
                                        logger::debug("Ctrl+V: Pasting {} characters from clipboard",
                                                      clipboardText.length());
                                        std::string escapedText = EscapeForJS(clipboardText);
                                        if (escapedText.empty() && !clipboardText.empty()) {
                                            logger::warn("Failed to escape clipboard text, paste cancelled");
                                        } else if (!escapedText.empty()) {
                                            std::string script =
                                                "document.execCommand('insertText', false, '" + escapedText + "')";
                                            Communication::Invoke(focusedViewId, script.c_str());
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    logger::error("Exception during paste operation: {}", e.what());
                                }
                            }
                            handledByUI = true;
                            break;
                        }

                        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C') {
                            if (g_ultralightThreadExecutor && g_viewsMap && g_viewsMapMutex) {
                                auto* viewsMap = g_viewsMap;
                                auto* viewsMapMutex = g_viewsMapMutex;
                                g_ultralightThreadExecutor->submit([viewId = focusedViewId, viewsMap, viewsMapMutex]() {
                                    try {
                                        std::shared_ptr<Core::PrismaView> viewData;
                                        {
                                            std::shared_lock lock(*viewsMapMutex);
                                            auto it = viewsMap->find(viewId);
                                            if (it != viewsMap->end()) {
                                                viewData = it->second;
                                            }
                                        }

                                        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) ||
                                            !viewData->ultralightView) {
                                            return;
                                        }

                                        ultralight::String result = viewData->ultralightView->EvaluateScript(
                                            "window.getSelection().toString()", nullptr);
                                        if (viewData->isDestroying.load(std::memory_order_acquire)) {
                                            return;
                                        }

                                        std::string selectedText = result.utf8().data();
                                        if (selectedText.size() > kMaxClipboardSize) {
                                            logger::warn(
                                                "Selected text too large to copy: {} bytes (max: {} bytes)",
                                                selectedText.size(), kMaxClipboardSize);
                                            return;
                                        }
                                        if (!selectedText.empty()) {
                                            F4SE::GetTaskInterface()->AddTask([text = std::move(selectedText)]() {
                                                SetClipboardText(text);
                                                logger::debug("Ctrl+C: Copied {} characters to clipboard",
                                                              text.length());
                                            });
                                        }
                                    } catch (const std::exception& e) {
                                        logger::error("Exception during copy operation: {}", e.what());
                                    }
                                });
                            }
                            handledByUI = true;
                            break;
                        }

                        QueueInputForView(
                            focusedViewId,
                            WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_RawKeyDown, wParam, lParam));
                        handledByUI = true;
                        break;
                    }
                    case WM_KEYUP:
                        QueueInputForView(
                            focusedViewId,
                            WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_KeyUp, wParam, lParam));
                        handledByUI = true;
                        break;
                    case WM_CHAR: {
                        if (g_isFocusedTextInputActive.load()) {
                            handledByUI = true;
                            wchar_t ch = static_cast<wchar_t>(wParam);
                            if (IsHighSurrogate(ch)) {
                                g_pendingHighSurrogate = ch;
                                break;
                            }

                            std::wstring committedText;
                            if (IsLowSurrogate(ch) && g_pendingHighSurrogate != 0) {
                                committedText.push_back(g_pendingHighSurrogate);
                                committedText.push_back(ch);
                                g_pendingHighSurrogate = 0;
                            } else {
                                g_pendingHighSurrogate = 0;
                                if (ShouldQueueChar(ch)) {
                                    committedText.push_back(ch);
                                }
                            }
                            QueueCommittedCharEvent(committedText, lParam);
                        } else {
                            g_pendingHighSurrogate = 0;
                        }
                        break;
                    }
                    case WM_UNICHAR: {
                        if (wParam == UNICODE_NOCHAR) {
                            return TRUE;
                        }
                        if (g_isFocusedTextInputActive.load()) {
                            handledByUI = true;
                            std::wstring committedText = ConvertCodePointToUtf16(static_cast<UINT>(wParam));
                            if (!committedText.empty() && ShouldQueueChar(committedText[0])) {
                                g_pendingHighSurrogate = 0;
                                QueueCommittedCharEvent(committedText, lParam);
                            }
                        }
                        break;
                    }
                    case WM_MOUSEMOVE: {
                        ultralight::MouseEvent event;
                        event.type = ultralight::MouseEvent::kType_MouseMoved;
                        event.x = GET_X_LPARAM(lParam);
                        event.y = GET_Y_LPARAM(lParam);
                        event.button = ultralight::MouseEvent::kButton_None;
                        QueueInputForView(focusedViewId, event);
                        break;
                    }
                    case WM_LBUTTONDOWN:
                        handledByUI = HandleMouseButtonDown(focusedViewId, 0, ultralight::MouseEvent::kButton_Left,
                                                            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                        break;
                    case WM_LBUTTONUP:
                        handledByUI = HandleMouseButtonUp(focusedViewId, 0, ultralight::MouseEvent::kButton_Left,
                                                          GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                        break;
                    case WM_RBUTTONDOWN:
                        handledByUI = HandleMouseButtonDown(focusedViewId, 1, ultralight::MouseEvent::kButton_Right,
                                                            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                        break;
                    case WM_RBUTTONUP:
                        handledByUI = HandleMouseButtonUp(focusedViewId, 1, ultralight::MouseEvent::kButton_Right,
                                                          GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                        break;
                    case WM_MBUTTONDOWN:
                        handledByUI = HandleMouseButtonDown(focusedViewId, 2, ultralight::MouseEvent::kButton_Middle,
                                                            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                        break;
                    case WM_MBUTTONUP:
                        handledByUI = HandleMouseButtonUp(focusedViewId, 2, ultralight::MouseEvent::kButton_Middle,
                                                          GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                        break;
                    case WM_MOUSEWHEEL: {
                        int scrollPixelSize = 28;
                        auto viewData = FindLiveView(focusedViewId);
                        if (viewData) {
                            scrollPixelSize = viewData->scrollingPixelSize;
                        }

                        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                        const int scrollAmount =
                            (delta > 0 ? 1 : -1) * kScrollLinesPerWheelDelta * scrollPixelSize;
                        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                        ScreenToClient(hwnd, &point);

                        ScrollEventWithPosition scroll;
                        scroll.event.type = ultralight::ScrollEvent::kType_ScrollByPixel;
                        scroll.event.delta_x = 0;
                        scroll.event.delta_y = scrollAmount;
                        scroll.mouseX = point.x;
                        scroll.mouseY = point.y;
                        QueueInputForView(focusedViewId, scroll);
                        handledByUI = true;
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

        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(hwnd, SubclassProc, subclassId);
            if (subclassId == kSubclassId) {
                g_wndProcInstalled.store(false);
            }
        }

        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor,
                    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                    std::shared_mutex* viewsMapMutex) {
        g_hWnd = gameHwnd;
        g_ultralightThreadExecutor = coreExecutor;
        g_viewsMap = viewsMap;
        g_viewsMapMutex = viewsMapMutex;
        g_isAnyInputCaptureActive = false;
        g_isFocusedTextInputActive = false;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            g_currentlyFocusedViewId = 0;
        }
        {
            std::lock_guard lock(g_eventQueueMutex);
            g_eventQueue.clear();
        }

        g_pointerDragOwnedByUI = false;
        g_mouseButtonStates[0] = false;
        g_mouseButtonStates[1] = false;
        g_mouseButtonStates[2] = false;
        g_pendingHighSurrogate = 0;

        logger::info("PrismaUI::InputHandler Initialized with HWND: {}", static_cast<void*>(g_hWnd));

        g_imeHelper.SetCallbacks(
            [](const std::string& text) { return EscapeForJS(text); },
            [](const std::wstring& text, LPARAM lParam) { QueueCommittedCharEvent(text, lParam); },
            [](const wchar_t* text, int length) { return ConvertUtf16ToUtf8(text, length); });
        g_imeHelper.SetContext({g_hWnd, g_viewsMap, g_viewsMapMutex, &g_focusedViewIdMutex,
                                &g_currentlyFocusedViewId, &g_isAnyInputCaptureActive, &g_isFocusedTextInputActive});
        g_imeHelper.SetExecutor(g_ultralightThreadExecutor);
        g_imeHelper.Initialize(g_hWnd);

        // Fallout 4 does not expose a usable BSTEventSource<InputEvent*> here, so mouse input uses WndProc subclassing.
        logger::info("Mouse events will be handled via WndProc subclass.");
    }

    bool InstallWndProcHook() {
        std::lock_guard lock(g_wndProcMutex);
        if (g_wndProcInstalled.load()) {
            logger::debug("WndProc subclass already installed");
            return true;
        }
        if (!g_hWnd) {
            logger::error("Cannot install WndProc subclass: HWND is null");
            return false;
        }
        if (!IsWindow(g_hWnd)) {
            logger::error("HWND {:p} is not a valid window", static_cast<void*>(g_hWnd));
            return false;
        }

        logger::debug("Attempting to install subclass on HWND: {:p}", static_cast<void*>(g_hWnd));
        SetLastError(0);
        if (!SetWindowSubclass(g_hWnd, SubclassProc, kSubclassId, 0)) {
            const DWORD error = GetLastError();
            logger::error("Failed to install WndProc subclass. Error: {} (0x{:X})", error, error);
            return false;
        }

        g_wndProcInstalled.store(true);
        logger::info("WndProc subclass installed successfully on HWND: {:p}", static_cast<void*>(g_hWnd));
        return true;
    }

    void UninstallWndProcHook() {
        std::lock_guard lock(g_wndProcMutex);
        if (!g_wndProcInstalled.exchange(false)) {
            return;
        }
        if (g_hWnd) {
            RemoveWindowSubclass(g_hWnd, SubclassProc, kSubclassId);
            logger::info("WndProc subclass removed");
        }
    }

    void EnableInputCapture(const Core::PrismaViewId& viewId) {
        if (!FindLiveView(viewId)) {
            logger::warn("EnableInputCapture called for invalid or destroying View [{}].", viewId);
            return;
        }

        {
            std::lock_guard lock(g_focusedViewIdMutex);
            if (g_currentlyFocusedViewId != viewId) {
                g_currentlyFocusedViewId = viewId;
                logger::debug("PrismaUI Input Capture focused on View [{}].", viewId);
            }
        }

        DiscardQueuedEventsExcept(viewId);
        g_pointerDragOwnedByUI = false;
        g_mouseButtonStates[0] = false;
        g_mouseButtonStates[1] = false;
        g_mouseButtonStates[2] = false;
        g_pendingHighSurrogate = 0;

        if (!g_isAnyInputCaptureActive.exchange(true)) {
            logger::debug("PrismaUI Input Capture System Enabled for View [{}].", viewId);
        }

        g_isFocusedTextInputActive.store(false);
        g_imeHelper.SetAssociation(false);

        Communication::RegisterJSListener(viewId, kImeFocusCallbackName, [viewId](std::string focused) {
            if (GetFocusedViewIdSnapshot() != viewId || !g_isAnyInputCaptureActive.load()) {
                return;
            }

            const bool isTextInputFocused = focused == "1";
            g_isFocusedTextInputActive.store(isTextInputFocused);
            g_imeHelper.SetAssociation(isTextInputFocused);

            if (!isTextInputFocused) {
                if (g_ultralightThreadExecutor) {
                    g_ultralightThreadExecutor->submit([viewId]() { g_imeHelper.ClearStateInJS(viewId); });
                } else {
                    g_imeHelper.ClearStateInJS(viewId);
                }
            }
        });
        InstallImeFocusTrackingForView(viewId);
    }

    void DisableInputCapture(const Core::PrismaViewId& viewIdToUnfocus) {
        bool disableSystem = false;
        Core::PrismaViewId previouslyFocused = 0;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            previouslyFocused = g_currentlyFocusedViewId;
            if (viewIdToUnfocus == 0 || viewIdToUnfocus == g_currentlyFocusedViewId) {
                disableSystem = g_isAnyInputCaptureActive.load();
                g_currentlyFocusedViewId = 0;
            }
        }

        if (viewIdToUnfocus != 0) {
            DiscardQueuedEventsForView(viewIdToUnfocus);
        }

        if (!disableSystem) {
            if (viewIdToUnfocus != 0) {
                logger::debug("DisableInputCapture ignored stale View [{}]; current focus is View [{}].",
                              viewIdToUnfocus, previouslyFocused);
            }
            return;
        }

        if (!g_isAnyInputCaptureActive.exchange(false)) {
            return;
        }

        g_isFocusedTextInputActive.store(false);
        g_imeHelper.SetAssociation(false);
        DiscardQueuedEventsForView(0);
        g_pointerDragOwnedByUI = false;
        g_mouseButtonStates[0] = false;
        g_mouseButtonStates[1] = false;
        g_mouseButtonStates[2] = false;
        g_pendingHighSurrogate = 0;
        logger::debug("PrismaUI Input Capture System Disabled (was active for View [{}]).", previouslyFocused);

        if (!g_ultralightThreadExecutor || previouslyFocused == 0 || !g_viewsMap || !g_viewsMapMutex) {
            return;
        }

        auto* viewsMap = g_viewsMap;
        auto* viewsMapMutex = g_viewsMapMutex;
        g_ultralightThreadExecutor->submit([viewId = previouslyFocused, viewsMap, viewsMapMutex]() {
            std::shared_ptr<Core::PrismaView> viewData;
            {
                std::shared_lock lock(*viewsMapMutex);
                auto it = viewsMap->find(viewId);
                if (it != viewsMap->end()) {
                    viewData = it->second;
                }
            }

            if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) {
                return;
            }

            ultralight::MouseEvent resetEvent;
            resetEvent.type = ultralight::MouseEvent::kType_MouseMoved;
            resetEvent.x = 0;
            resetEvent.y = 0;
            resetEvent.button = ultralight::MouseEvent::kButton_None;
            viewData->ultralightView->FireMouseEvent(resetEvent);
        });
    }

    void ClearImeState(const Core::PrismaViewId& viewId) {
        if (viewId == 0) {
            return;
        }

        if (GetFocusedViewIdSnapshot() == viewId) {
            g_isFocusedTextInputActive.store(false);
            g_imeHelper.SetAssociation(false);
        }
        g_imeHelper.ClearStateInJS(viewId);
    }

    bool IsAnyInputCaptureActive() { return g_isAnyInputCaptureActive.load(); }

    Core::PrismaViewId GetFocusedViewId() { return GetFocusedViewIdSnapshot(); }

    int GetLastCursorX() { return g_lastCursorX.load(); }
    int GetLastCursorY() { return g_lastCursorY.load(); }

    bool IsInputCaptureActiveForView(const Core::PrismaViewId& viewId) {
        const Core::PrismaViewId currentFocused = GetFocusedViewIdSnapshot();
        if (viewId == 0) {
            return g_isAnyInputCaptureActive.load();
        }
        return g_isAnyInputCaptureActive.load() && currentFocused == viewId;
    }

    void ProcessEvents() {
        if (!g_ultralightThreadExecutor || !g_viewsMap || !g_viewsMapMutex) {
            return;
        }

        const Core::PrismaViewId focusedViewId = GetFocusedViewIdSnapshot();
        std::vector<QueuedInputEvent> queuedEvents;
        {
            std::lock_guard lock(g_eventQueueMutex);
            if (g_eventQueue.empty()) {
                return;
            }
            queuedEvents.swap(g_eventQueue);
        }

        if (focusedViewId == 0 || !g_isAnyInputCaptureActive.load()) {
            return;
        }

        std::vector<InputEvent> eventsToProcess;
        eventsToProcess.reserve(queuedEvents.size());
        for (auto& queued : queuedEvents) {
            if (queued.viewId == focusedViewId) {
                eventsToProcess.push_back(std::move(queued.event));
            }
        }
        if (eventsToProcess.empty()) {
            return;
        }

        auto* viewsMap = g_viewsMap;
        auto* viewsMapMutex = g_viewsMapMutex;
        try {
            g_ultralightThreadExecutor->submit(
                [viewId = focusedViewId, events = std::move(eventsToProcess), viewsMap, viewsMapMutex]() mutable {
                    if (!g_isAnyInputCaptureActive.load() || GetFocusedViewIdSnapshot() != viewId) {
                        return;
                    }

                    std::shared_ptr<Core::PrismaView> viewData;
                    {
                        std::shared_lock lock(*viewsMapMutex);
                        auto it = viewsMap->find(viewId);
                        if (it != viewsMap->end()) {
                            viewData = it->second;
                        }
                    }

                    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) ||
                        !viewData->ultralightView) {
                        return;
                    }

                    ultralight::RefPtr<ultralight::View> mainView = viewData->ultralightView;
                    ultralight::RefPtr<ultralight::View> inspectorView = viewData->inspectorView;

                    for (const auto& event : events) {
                        if (viewData->isDestroying.load(std::memory_order_acquire) ||
                            !g_isAnyInputCaptureActive.load() || GetFocusedViewIdSnapshot() != viewId) {
                            break;
                        }

                        std::visit(
                            [&mainView, &inspectorView, &viewData](const auto& value) {
                                using EventType = std::decay_t<decltype(value)>;
                                if constexpr (std::is_same_v<EventType, ultralight::MouseEvent>) {
                                    bool mouseOverInspector = false;
                                    if (inspectorView && viewData->inspectorVisible.load()) {
                                        const float x = static_cast<float>(value.x);
                                        const float y = static_cast<float>(value.y);
                                        const float inspectorX = viewData->inspectorPosX;
                                        const float inspectorY = viewData->inspectorPosY;
                                        const float inspectorWidth =
                                            static_cast<float>(viewData->inspectorDisplayWidth);
                                        const float inspectorHeight =
                                            static_cast<float>(viewData->inspectorDisplayHeight);
                                        mouseOverInspector =
                                            x >= inspectorX && x < inspectorX + inspectorWidth && y >= inspectorY &&
                                            y < inspectorY + inspectorHeight;
                                        viewData->inspectorPointerHover.store(mouseOverInspector);
                                    }

                                    if (mouseOverInspector) {
                                        ultralight::MouseEvent inspectorEvent = value;
                                        inspectorEvent.x = value.x - static_cast<int>(viewData->inspectorPosX);
                                        inspectorEvent.y = value.y - static_cast<int>(viewData->inspectorPosY);
                                        inspectorView->FireMouseEvent(inspectorEvent);
                                    } else {
                                        mainView->FireMouseEvent(value);
                                    }
                                } else if constexpr (std::is_same_v<EventType, ScrollEventWithPosition>) {
                                    bool scrollOverInspector = false;
                                    if (inspectorView && viewData->inspectorVisible.load()) {
                                        const float x = static_cast<float>(value.mouseX);
                                        const float y = static_cast<float>(value.mouseY);
                                        const float inspectorX = viewData->inspectorPosX;
                                        const float inspectorY = viewData->inspectorPosY;
                                        const float inspectorWidth =
                                            static_cast<float>(viewData->inspectorDisplayWidth);
                                        const float inspectorHeight =
                                            static_cast<float>(viewData->inspectorDisplayHeight);
                                        scrollOverInspector =
                                            x >= inspectorX && x < inspectorX + inspectorWidth && y >= inspectorY &&
                                            y < inspectorY + inspectorHeight;
                                    }
                                    if (scrollOverInspector) {
                                        inspectorView->FireScrollEvent(value.event);
                                    } else {
                                        mainView->FireScrollEvent(value.event);
                                    }
                                } else if constexpr (std::is_same_v<EventType, ultralight::KeyEvent>) {
                                    if (inspectorView && viewData->inspectorVisible.load() && inspectorView->HasFocus()) {
                                        inspectorView->FireKeyEvent(value);
                                    } else {
                                        mainView->FireKeyEvent(value);
                                    }
                                }
                            },
                            event);
                    }
                });
        } catch (const std::exception& e) {
            logger::error("ProcessEvents: Failed to submit input batch for View [{}]: {}", focusedViewId, e.what());
        }
    }

    void RegisterOverlayClickHandler(OverlayClickCallback callback) {
        if (!callback) {
            return;
        }
        std::lock_guard lock(g_overlayClickMutex);
        g_overlayClickHandlers.push_back(std::move(callback));
    }

    void Shutdown() {
        DisableInputCapture(0);
        {
            std::lock_guard lock(g_eventQueueMutex);
            g_eventQueue.clear();
        }
        {
            std::lock_guard lock(g_overlayClickMutex);
            g_overlayClickHandlers.clear();
        }

        UninstallWndProcHook();
        g_imeHelper.Shutdown(g_hWnd);

        g_hWnd = nullptr;
        g_ultralightThreadExecutor = nullptr;
        g_viewsMap = nullptr;
        g_viewsMapMutex = nullptr;
        g_isFocusedTextInputActive = false;
        g_pointerDragOwnedByUI = false;
        g_pendingHighSurrogate = 0;
        logger::info("PrismaUI::InputHandler Shutdown.");
    }
}
