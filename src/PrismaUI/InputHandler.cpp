#include "InputHandler.h"

#include <commctrl.h>

#include "Communication.h"
#include "Core.h"
#include "ImeHelper.h"
#include "Utils/Encoding.h"
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

    // Click-through: a mouse button press over a TRANSPARENT part of the focused
    // view (e.g. where the game renders a 3D model behind the UI) is passed to the
    // game instead of being captured. A drag that started over real UI content stays
    // captured until release, so UI drags don't break when the cursor leaves content.
    bool g_pointerDragOwnedByUI = false;

    // True if the focused view has visible (opaque-enough) UI content at client (x,y).
    static bool IsCursorOverContent(const Core::PrismaViewId& viewId, int x, int y) {
        if (viewId == 0 || !g_viewsMap || !g_viewsMapMutex) return true;
        std::shared_ptr<Core::PrismaView> vd;
        {
            std::shared_lock<std::shared_mutex> lock(*g_viewsMapMutex);
            auto it = g_viewsMap->find(viewId);
            if (it != g_viewsMap->end()) vd = it->second;
        }
        if (!vd) return true;
        std::lock_guard<std::mutex> lock(vd->bufferMutex);
        if (vd->pixelBuffer.empty() || vd->bufferWidth == 0 || vd->bufferHeight == 0 || vd->bufferStride == 0)
            return true;  // no frame captured yet → default to capturing input
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= vd->bufferWidth ||
            static_cast<uint32_t>(y) >= vd->bufferHeight)
            return false;  // outside the view → let it pass through
        const size_t off = static_cast<size_t>(y) * vd->bufferStride + static_cast<size_t>(x) * 4u + 3u;  // BGRA, alpha at +3
        if (off >= vd->pixelBuffer.size()) return true;
        return std::to_integer<std::uint8_t>(vd->pixelBuffer[off]) >= 12;  // alpha threshold for "real UI"
    }

    const int SCROLL_LINES_PER_WHEEL_DELTA = 1;

    // Clipboard safety limits
    constexpr size_t MAX_CLIPBOARD_SIZE = 1024 * 1024;  // 1MB max
    constexpr size_t MAX_CLIPBOARD_CHARS = 200000;      // 200K characters max

    bool g_mouseButtonStates[3] = {false, false, false};
    wchar_t g_pendingHighSurrogate = 0;

    static std::atomic<int> g_lastCursorX = 0;
    static std::atomic<int> g_lastCursorY = 0;

    // WndProc subclass state
    static std::atomic<bool> g_wndProcInstalled = false;
    static constexpr UINT_PTR SUBCLASS_ID = 0x505249534D41;  // "PRISMA" in hex
    static std::mutex g_wndProcMutex;                        // Thread-safe installation

    // Overlay hit-test callbacks: run for every WM_LBUTTONDOWN before Scaleform routing.
    static std::mutex g_overlayClickMutex;
    static std::vector<OverlayClickCallback> g_overlayClickHandlers;

    static ImeHelper g_imeHelper;
    static std::atomic<bool> g_isFocusedTextInputActive = false;

    constexpr const char* IME_FOCUS_CALLBACK_NAME = "__prismaNativeImeFocusChanged";

    std::string BuildImeFocusTrackingScript() {
        const std::string callbackName = IME_FOCUS_CALLBACK_NAME;
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

        auto install = [viewId]() {
            std::shared_ptr<Core::PrismaView> viewData = nullptr;
            {
                std::shared_lock lock(*g_viewsMapMutex);
                auto it = g_viewsMap->find(viewId);
                if (it != g_viewsMap->end()) {
                    viewData = it->second;
                }
            }

            if (!viewData || !viewData->ultralightView || !viewData->isLoadingFinished.load()) {
                return;
            }

            Communication::BindJSCallbacks(viewId);

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
            return;
        }

        g_ultralightThreadExecutor->submit(install);
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
            unsigned char c = static_cast<unsigned char>(text[i]);

            if (c >= 0x80) {
                // Check for Unicode line/paragraph separators (U+2028 = E2 80 A8, U+2029 = E2 80 A9)
                if (i + 2 < text.size() && c == 0xE2 && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                    (static_cast<unsigned char>(text[i + 2]) == 0xA8 ||
                     static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
                    escaped += "\\u202";
                    escaped += (text[i + 2] == 0xA8) ? '8' : '9';
                    i += 2;
                    continue;
                }
                escaped += c;
                continue;
            }

            switch (c) {
                case '\'':
                    escaped += "\\'";
                    break;
                case '\"':
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
                    if (c < 0x20) {
                        logger::trace("Filtered control character: 0x{:02X}", static_cast<int>(c));
                    } else {
                        escaped += c;
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

        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (!hData) {
            CloseClipboard();
            return "";
        }

        wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
        if (!pszText) {
            CloseClipboard();
            return "";
        }

        SIZE_T dataSize = GlobalSize(hData);
        if (dataSize > MAX_CLIPBOARD_SIZE) {
            logger::warn("Clipboard text too large: {} bytes (max: {} bytes). Truncating.", dataSize,
                         MAX_CLIPBOARD_SIZE);
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        int utf8Length = WideCharToMultiByte(CP_UTF8, 0, pszText, -1, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) {
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        size_t charCount = wcslen(pszText);
        if (charCount > MAX_CLIPBOARD_CHARS) {
            logger::warn("Clipboard text too long: {} characters (max: {} characters). Truncating.", charCount,
                         MAX_CLIPBOARD_CHARS);
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        std::string result;
        try {
            result.resize(utf8Length - 1);
            WideCharToMultiByte(CP_UTF8, 0, pszText, -1, &result[0], utf8Length, nullptr, nullptr);
        } catch (const std::exception& e) {
            logger::error("Failed to allocate memory for clipboard text: {}", e.what());
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        GlobalUnlock(hData);
        CloseClipboard();

        return result;
    }

    void SetClipboardText(const std::string& text) {
        if (text.size() > MAX_CLIPBOARD_SIZE) {
            logger::warn("Text too large to copy to clipboard: {} bytes (max: {} bytes)", text.size(),
                         MAX_CLIPBOARD_SIZE);
            return;
        }

        if (!OpenClipboard(g_hWnd)) {
            logger::warn("Failed to open clipboard for writing");
            return;
        }

        EmptyClipboard();
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wideLength <= 0) {
            CloseClipboard();
            return;
        }

        HGLOBAL hMem = nullptr;
        try {
            hMem = GlobalAlloc(GMEM_MOVEABLE, wideLength * sizeof(wchar_t));
            if (!hMem) {
                logger::error("Failed to allocate global memory for clipboard");
                CloseClipboard();
                return;
            }

            wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
            if (!pMem) {
                GlobalFree(hMem);
                CloseClipboard();
                return;
            }

            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, wideLength);
            GlobalUnlock(hMem);

            if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                GlobalFree(hMem);
                logger::warn("Failed to set clipboard data");
            }
        } catch (const std::exception& e) {
            logger::error("Exception while setting clipboard text: {}", e.what());
            if (hMem) {
                GlobalFree(hMem);
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

        int utf8Length = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) {
            return "";
        }

        std::string result;
        try {
            result.resize(utf8Length);
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

        std::string utf8Text = ConvertUtf16ToUtf8(utf16Text.data(), static_cast<int>(utf16Text.size()));
        if (utf8Text.empty()) {
            logger::warn("Failed to convert committed text to UTF-8");
            return;
        }

        ultralight::KeyEvent charEvent;
        charEvent.type = ultralight::KeyEvent::kType_Char;
        WinKeyHandler::GetUltralightModifiers(charEvent);

        ultralight::String ulText = ultralight::String(utf8Text.c_str());
        charEvent.text = ulText;
        charEvent.unmodified_text = ulText;
        charEvent.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
        charEvent.native_key_code = 0;
        charEvent.key_identifier = "";
        charEvent.is_keypad = false;
        charEvent.is_auto_repeat = (HIWORD(lParam) & KF_REPEAT) == KF_REPEAT;
        charEvent.is_system_key = false;

        std::lock_guard lock(g_eventQueueMutex);
        g_eventQueue.emplace_back(charEvent);
    }

    std::wstring ConvertCodePointToUtf16(UINT codePoint) {
        if (codePoint > 0x10FFFF) {
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

    // Mouse events are handled via WndProc (SubclassProc) below.
    // F4 does not expose BSTEventSource<InputEvent*> from BSInputDeviceManager,
    // so the SKSE-style event sink pattern is not used here.

    LRESULT CALLBACK SubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                                  DWORD_PTR /*dwRefData*/) {
        LRESULT imeControlResult = 0;
        if (g_imeHelper.HandleControlMessage(hwnd, uMsg, wParam, lParam, &imeControlResult)) {
            return imeControlResult;
        }

        if (uMsg == WM_IME_SETCONTEXT) {
            LPARAM imeLParam = lParam;
            g_imeHelper.ModifySetContextLParam(&imeLParam, uMsg);
            lParam = imeLParam;
        }

        // Track mouse position for cursor rendering regardless of capture state.
        if (uMsg == WM_MOUSEMOVE) {
            g_lastCursorX.store(static_cast<int>(LOWORD(lParam)));
            g_lastCursorY.store(static_cast<int>(HIWORD(lParam)));
        }

        // Overlay hit-test: runs before PrismaUI capture and before Scaleform/game routing.
        // lParam for WM_LBUTTONDOWN is already in client coordinates.
        if (uMsg == WM_LBUTTONDOWN) {
            const int clickX = static_cast<int>(LOWORD(lParam));
            const int clickY = static_cast<int>(HIWORD(lParam));
            std::lock_guard<std::mutex> lock(g_overlayClickMutex);
            for (auto& cb : g_overlayClickHandlers) {
                if (cb(clickX, clickY)) return 0; // consumed — Scaleform won't see it
            }
        }

        if (g_isAnyInputCaptureActive.load()) {
            // WM_SETCURSOR fires on every mouse move; return TRUE to prevent Windows restoring cursor
            if (uMsg == WM_SETCURSOR) {
                SetCursor(NULL);
                return TRUE;
            }

            bool handledByUI = false;
            Core::PrismaViewId focusedViewIdCopy;
            {
                std::lock_guard lock(g_focusedViewIdMutex);
                focusedViewIdCopy = g_currentlyFocusedViewId;
            }

            if (focusedViewIdCopy != 0) {
                if (g_imeHelper.HandleMessage(hwnd, uMsg, wParam, lParam, focusedViewIdCopy, &handledByUI)) {
                    if (handledByUI) {
                        return 0;
                    }
                }

                switch (uMsg) {
                    case WM_KEYDOWN: {
                        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'V') {
                            const bool viewHasInputFieldFocus = g_isFocusedTextInputActive.load();
                            if (viewHasInputFieldFocus) {
                                try {
                                    std::string clipboardText = GetClipboardText();
                                    if (!clipboardText.empty()) {
                                        logger::debug("Ctrl+V: Pasting {} characters from clipboard",
                                                      clipboardText.length());

                                        // Escape text for JavaScript string
                                        std::string escapedText = EscapeForJS(clipboardText);

                                        if (escapedText.empty() && !clipboardText.empty()) {
                                            logger::warn("Failed to escape clipboard text, paste cancelled");
                                        } else if (!escapedText.empty()) {
                                            // Use JavaScript execCommand to insert text properly (handles UTF-8)
                                            std::string script =
                                                "document.execCommand('insertText', false, '" + escapedText + "')";

                                            PrismaUI::Communication::Invoke(focusedViewIdCopy, script.c_str());
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    logger::error("Exception during paste operation: {}", e.what());
                                }
                            }
                            handledByUI = true;
                            break;
                        }

                        // Handle Ctrl+C (Copy)
                        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C') {
                            // Execute JavaScript to get selected text on ultralightThread
                            if (g_ultralightThreadExecutor && g_viewsMap && g_viewsMapMutex) {
                                g_ultralightThreadExecutor->submit([viewId = focusedViewIdCopy]() {
                                    try {
                                        std::shared_ptr<Core::PrismaView> viewData = nullptr;
                                        {
                                            std::shared_lock lock(*g_viewsMapMutex);
                                            auto it = g_viewsMap->find(viewId);
                                            if (it != g_viewsMap->end()) {
                                                viewData = it->second;
                                            }
                                        }

                                        if (viewData && viewData->ultralightView) {
                                            ultralight::String script = "window.getSelection().toString()";
                                            ultralight::String result =
                                                viewData->ultralightView->EvaluateScript(script, nullptr);
                                            std::string selectedText = result.utf8().data();

                                            // Check size before copying
                                            if (selectedText.size() > MAX_CLIPBOARD_SIZE) {
                                                logger::warn(
                                                    "Selected text too large to copy: {} bytes (max: {} bytes)",
                                                    selectedText.size(), MAX_CLIPBOARD_SIZE);
                                                return;
                                            }

                                            if (!selectedText.empty()) {
                                                // Copy to clipboard on Skyrim main thread to avoid blocking
                                                F4SE::GetTaskInterface()->AddTask([text = selectedText]() {
                                                    SetClipboardText(text);
                                                    logger::debug("Ctrl+C: Copied {} characters to clipboard",
                                                                  text.length());
                                                });
                                            }
                                        }
                                    } catch (const std::exception& e) {
                                        logger::error("Exception during copy operation: {}", e.what());
                                    }
                                });
                            }
                            handledByUI = true;
                            break;
                        }

                        // Normal key processing
                        ultralight::KeyEvent keyDownEvent =
                            WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_RawKeyDown, wParam, lParam);
                        {
                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(keyDownEvent);
                        }
                        handledByUI = true;
                        break;
                    }
                    case WM_KEYUP: {
                        ultralight::KeyEvent ev =
                            WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_KeyUp, wParam, lParam);
                        {
                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(ev);
                        }
                        handledByUI = true;
                        break;
                    }
                    case WM_CHAR: {
                        const bool viewHasInputFieldFocus = g_isFocusedTextInputActive.load();
                        if (viewHasInputFieldFocus) {
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

                        const bool viewHasInputFieldFocus = g_isFocusedTextInputActive.load();
                        if (viewHasInputFieldFocus) {
                            handledByUI = true;

                            std::wstring committedText = ConvertCodePointToUtf16(static_cast<UINT>(wParam));
                            if (!committedText.empty() && ShouldQueueChar(committedText[0])) {
                                g_pendingHighSurrogate = 0;
                                QueueCommittedCharEvent(committedText, lParam);
                            }
                        }
                        break;
                    }
                    // Mouse move
                    case WM_MOUSEMOVE: {
                        ultralight::MouseEvent ev;
                        ev.type = ultralight::MouseEvent::kType_MouseMoved;
                        ev.x = static_cast<int>(LOWORD(lParam));
                        ev.y = static_cast<int>(HIWORD(lParam));
                        ev.button = ultralight::MouseEvent::kButton_None;
                        std::lock_guard lock(g_eventQueueMutex);
                        g_eventQueue.emplace_back(ev);
                        break;
                    }
                    // Mouse buttons. Each press alpha-tests the cursor: over UI
                    // content it is captured (queued + handledByUI); over a
                    // transparent region it falls through to the game (handledByUI
                    // stays false), letting the player interact with the 3D model
                    // behind the view. Releases honour the press's owner so UI drags
                    // still receive their mouseup after the cursor leaves content.
                    case WM_LBUTTONDOWN: {
                        const bool wasAnyDown = g_mouseButtonStates[0] || g_mouseButtonStates[1] || g_mouseButtonStates[2];
                        g_mouseButtonStates[0] = true;
                        const int mx = static_cast<int>(LOWORD(lParam));
                        const int my = static_cast<int>(HIWORD(lParam));
                        if (!wasAnyDown) g_pointerDragOwnedByUI = IsCursorOverContent(focusedViewIdCopy, mx, my);
                        if (!g_pointerDragOwnedByUI) break;  // pass to game
                        ultralight::MouseEvent ev;
                        ev.type = ultralight::MouseEvent::kType_MouseDown;
                        ev.x = mx; ev.y = my; ev.button = ultralight::MouseEvent::kButton_Left;
                        { std::lock_guard lock(g_eventQueueMutex); g_eventQueue.emplace_back(ev); }
                        handledByUI = true;
                        break;
                    }
                    case WM_LBUTTONUP: {
                        g_mouseButtonStates[0] = false;
                        const bool capture = g_pointerDragOwnedByUI;
                        if (capture) {
                            ultralight::MouseEvent ev;
                            ev.type = ultralight::MouseEvent::kType_MouseUp;
                            ev.x = static_cast<int>(LOWORD(lParam));
                            ev.y = static_cast<int>(HIWORD(lParam));
                            ev.button = ultralight::MouseEvent::kButton_Left;
                            { std::lock_guard lock(g_eventQueueMutex); g_eventQueue.emplace_back(ev); }
                            handledByUI = true;
                        }
                        if (!(g_mouseButtonStates[0] || g_mouseButtonStates[1] || g_mouseButtonStates[2])) g_pointerDragOwnedByUI = false;
                        break;
                    }
                    case WM_RBUTTONDOWN: {
                        const bool wasAnyDown = g_mouseButtonStates[0] || g_mouseButtonStates[1] || g_mouseButtonStates[2];
                        g_mouseButtonStates[1] = true;
                        const int mx = static_cast<int>(LOWORD(lParam));
                        const int my = static_cast<int>(HIWORD(lParam));
                        if (!wasAnyDown) g_pointerDragOwnedByUI = IsCursorOverContent(focusedViewIdCopy, mx, my);
                        if (!g_pointerDragOwnedByUI) break;
                        ultralight::MouseEvent ev;
                        ev.type = ultralight::MouseEvent::kType_MouseDown;
                        ev.x = mx; ev.y = my; ev.button = ultralight::MouseEvent::kButton_Right;
                        { std::lock_guard lock(g_eventQueueMutex); g_eventQueue.emplace_back(ev); }
                        handledByUI = true;
                        break;
                    }
                    case WM_RBUTTONUP: {
                        g_mouseButtonStates[1] = false;
                        const bool capture = g_pointerDragOwnedByUI;
                        if (capture) {
                            ultralight::MouseEvent ev;
                            ev.type = ultralight::MouseEvent::kType_MouseUp;
                            ev.x = static_cast<int>(LOWORD(lParam));
                            ev.y = static_cast<int>(HIWORD(lParam));
                            ev.button = ultralight::MouseEvent::kButton_Right;
                            { std::lock_guard lock(g_eventQueueMutex); g_eventQueue.emplace_back(ev); }
                            handledByUI = true;
                        }
                        if (!(g_mouseButtonStates[0] || g_mouseButtonStates[1] || g_mouseButtonStates[2])) g_pointerDragOwnedByUI = false;
                        break;
                    }
                    case WM_MBUTTONDOWN: {
                        const bool wasAnyDown = g_mouseButtonStates[0] || g_mouseButtonStates[1] || g_mouseButtonStates[2];
                        g_mouseButtonStates[2] = true;
                        const int mx = static_cast<int>(LOWORD(lParam));
                        const int my = static_cast<int>(HIWORD(lParam));
                        if (!wasAnyDown) g_pointerDragOwnedByUI = IsCursorOverContent(focusedViewIdCopy, mx, my);
                        if (!g_pointerDragOwnedByUI) break;
                        ultralight::MouseEvent ev;
                        ev.type = ultralight::MouseEvent::kType_MouseDown;
                        ev.x = mx; ev.y = my; ev.button = ultralight::MouseEvent::kButton_Middle;
                        { std::lock_guard lock(g_eventQueueMutex); g_eventQueue.emplace_back(ev); }
                        handledByUI = true;
                        break;
                    }
                    case WM_MBUTTONUP: {
                        g_mouseButtonStates[2] = false;
                        const bool capture = g_pointerDragOwnedByUI;
                        if (capture) {
                            ultralight::MouseEvent ev;
                            ev.type = ultralight::MouseEvent::kType_MouseUp;
                            ev.x = static_cast<int>(LOWORD(lParam));
                            ev.y = static_cast<int>(HIWORD(lParam));
                            ev.button = ultralight::MouseEvent::kButton_Middle;
                            { std::lock_guard lock(g_eventQueueMutex); g_eventQueue.emplace_back(ev); }
                            handledByUI = true;
                        }
                        if (!(g_mouseButtonStates[0] || g_mouseButtonStates[1] || g_mouseButtonStates[2])) g_pointerDragOwnedByUI = false;
                        break;
                    }
                    // Scroll wheel
                    case WM_MOUSEWHEEL: {
                        int scrollPixelSize = 28;
                        Core::PrismaViewId focusedViewId;
                        {
                            std::lock_guard lock(g_focusedViewIdMutex);
                            focusedViewId = g_currentlyFocusedViewId;
                        }
                        if (focusedViewId != 0 && g_viewsMap && g_viewsMapMutex) {
                            std::shared_lock lock(*g_viewsMapMutex);
                            auto it = g_viewsMap->find(focusedViewId);
                            if (it != g_viewsMap->end() && it->second) {
                                scrollPixelSize = it->second->scrollingPixelSize;
                            }
                        }
                        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                        int scrollAmount = (delta > 0 ? 1 : -1) * SCROLL_LINES_PER_WHEEL_DELTA * scrollPixelSize;
                        POINT pt{ static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)) };
                        ScreenToClient(hwnd, &pt);
                        ScrollEventWithPosition scrollWithPos;
                        scrollWithPos.event.type = ultralight::ScrollEvent::kType_ScrollByPixel;
                        scrollWithPos.event.delta_x = 0;
                        scrollWithPos.event.delta_y = scrollAmount;
                        scrollWithPos.mouseX = pt.x;
                        scrollWithPos.mouseY = pt.y;
                        std::lock_guard lock(g_eventQueueMutex);
                        g_eventQueue.emplace_back(scrollWithPos);
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

        g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;

        logger::info("PrismaUI::InputHandler Initialized with HWND: {}", (void*)g_hWnd);

        g_imeHelper.SetCallbacks(
            [](const std::string& s) { return EscapeForJS(s); },
            [](const std::wstring& ws, LPARAM lp) { QueueCommittedCharEvent(ws, lp); },
            [](const wchar_t* p, int len) { return ConvertUtf16ToUtf8(p, len); });
        g_imeHelper.SetContext({g_hWnd, g_viewsMap, g_viewsMapMutex, &g_focusedViewIdMutex,
                                &g_currentlyFocusedViewId, &g_isAnyInputCaptureActive, &g_isFocusedTextInputActive});
        g_imeHelper.SetExecutor(g_ultralightThreadExecutor);
        g_imeHelper.Initialize(g_hWnd);

        // Mouse events handled via WndProc subclass (SubclassProc) - see InstallWndProcHook below.
        // F4's BSInputDeviceManager does not expose BSTEventSource<InputEvent*>.
        logger::info("Mouse events will be handled via WndProc subclass.");
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

        g_isFocusedTextInputActive.store(false);
        g_imeHelper.SetAssociation(false);

        Communication::RegisterJSListener(viewId, IME_FOCUS_CALLBACK_NAME, [viewId](std::string focused) {
            Core::PrismaViewId currentFocusedViewId = 0;
            {
                std::lock_guard lock(g_focusedViewIdMutex);
                currentFocusedViewId = g_currentlyFocusedViewId;
            }

            if (currentFocusedViewId != viewId) {
                return;
            }

            const bool isTextInputFocused = focused == "1";
            const bool isCaptureActive = g_isAnyInputCaptureActive.load();
            g_isFocusedTextInputActive.store(isTextInputFocused);
            g_imeHelper.SetAssociation(isCaptureActive && isTextInputFocused);

            if (!isTextInputFocused && g_ultralightThreadExecutor) {
                g_ultralightThreadExecutor->submit([viewId]() {
                    g_imeHelper.ClearStateInJS(viewId);
                });
            } else if (!isTextInputFocused) {
                g_imeHelper.ClearStateInJS(viewId);
            }
        });
        InstallImeFocusTrackingForView(viewId);

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
                g_isFocusedTextInputActive.store(false);
                g_imeHelper.SetAssociation(false);
                logger::debug("PrismaUI Input Capture System Disabled (was active for View [{}]).",
                              currentFocusedBeforeDisable);

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
        } else if (viewIdToUnfocus != 0) {
            logger::debug(
                "PrismaUI: DisableInputCapture called for View [{}] but View [{}] is/was focused. No change to system "
                "state, only unfocused ID removed if it matched.",
                viewIdToUnfocus, currentFocusedBeforeDisable);
        }
    }

    void ClearImeState(const Core::PrismaViewId& viewId) {
        if (viewId == 0) {
            return;
        }

        g_isFocusedTextInputActive.store(false);
        g_imeHelper.SetAssociation(false);
        g_imeHelper.ClearStateInJS(viewId);
    }

    bool IsAnyInputCaptureActive() { return g_isAnyInputCaptureActive.load(); }

    Core::PrismaViewId GetFocusedViewId() {
        std::lock_guard lock(g_focusedViewIdMutex);
        return g_currentlyFocusedViewId;
    }

    int GetLastCursorX() { return g_lastCursorX.load(); }
    int GetLastCursorY() { return g_lastCursorY.load(); }

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
                ultralight::View* inspectorView =
                    targetViewData->inspectorView ? targetViewData->inspectorView.get() : nullptr;

                for (const auto& event_variant : ev_queue) {
                    std::visit(
                        [ulView, inspectorView, &targetViewData](const auto& arg) {
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

                                    if (mouseX >= inspX && mouseX < (inspX + inspW) && mouseY >= inspY &&
                                        mouseY < (inspY + inspH)) {
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

                                    if (mouseX >= inspX && mouseX < (inspX + inspW) && mouseY >= inspY &&
                                        mouseY < (inspY + inspH)) {
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
                        },
                        event_variant);
                }
            }
        });
    }

    void RegisterOverlayClickHandler(OverlayClickCallback cb) {
        std::lock_guard<std::mutex> lock(g_overlayClickMutex);
        g_overlayClickHandlers.push_back(std::move(cb));
    }

    void Shutdown() {
        DisableInputCapture(0);
        {
            std::lock_guard lock(g_eventQueueMutex);
            g_eventQueue.clear();
        }

        UninstallWndProcHook();

        g_imeHelper.Shutdown(g_hWnd);

        g_hWnd = nullptr;
        g_ultralightThreadExecutor = nullptr;
        g_viewsMap = nullptr;
        g_viewsMapMutex = nullptr;
        g_isFocusedTextInputActive = false;
        logger::info("PrismaUI::InputHandler Shutdown.");
    }
}
