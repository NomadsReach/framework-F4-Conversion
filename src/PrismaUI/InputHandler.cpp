#include "InputHandler.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstddef>

#include "Communication.h"
#include "Core.h"
#include "GameThreadDispatcher.h"
#include "ImeHelper.h"
#include "Utils/Encoding.h"

#pragma comment(lib, "comctl32.lib")

namespace PrismaUI::InputHandler {
using namespace Core;

namespace {

struct CaptureSnapshot {
    Core::PrismaViewId viewId = 0;
    uint64_t generation = 0;
};

struct QueuedInputEvent {
    uint64_t generation = 0;
    InputEvent event;
};

HWND g_hWnd = nullptr;
SingleThreadExecutor* g_ultralightThreadExecutor = nullptr;
std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* g_viewsMap = nullptr;
std::shared_mutex* g_viewsMapMutex = nullptr;

Core::PrismaViewId g_currentlyFocusedViewId = 0;
std::mutex g_focusedViewIdMutex;
std::atomic<uint64_t> g_captureGeneration{1};
std::atomic<bool> g_isAnyInputCaptureActive{false};
std::atomic<bool> g_isFocusedTextInputActive{false};

std::mutex g_eventQueueMutex;
std::vector<QueuedInputEvent> g_eventQueue;

std::array<std::atomic<bool>, 3> g_mouseButtonStates{};
std::atomic<bool> g_pointerDragOwnedByUI{false};
wchar_t g_pendingHighSurrogate = 0;

std::atomic<int> g_lastCursorX{0};
std::atomic<int> g_lastCursorY{0};

std::atomic<bool> g_wndProcInstalled{false};
constexpr UINT_PTR SUBCLASS_ID = 0x505249534D41;
std::mutex g_wndProcMutex;

std::mutex g_overlayClickMutex;
std::vector<OverlayClickCallback> g_overlayClickHandlers;

ImeHelper g_imeHelper;

constexpr int SCROLL_LINES_PER_WHEEL_DELTA = 1;
constexpr size_t MAX_CLIPBOARD_SIZE = 1024 * 1024;
constexpr size_t MAX_CLIPBOARD_CHARS = 200000;
constexpr const char* IME_FOCUS_CALLBACK_NAME = "__prismaNativeImeFocusChanged";

CaptureSnapshot SnapshotCapture()
{
    std::lock_guard lock(g_focusedViewIdMutex);
    return {g_currentlyFocusedViewId, g_captureGeneration.load(std::memory_order_acquire)};
}

void ClearQueuedEvents()
{
    std::lock_guard lock(g_eventQueueMutex);
    g_eventQueue.clear();
}

void QueueEvent(uint64_t generation, InputEvent event)
{
    std::lock_guard lock(g_eventQueueMutex);
    if (generation != g_captureGeneration.load(std::memory_order_acquire)) return;
    g_eventQueue.push_back({generation, std::move(event)});
}

bool AnyMouseButtonDown()
{
    return g_mouseButtonStates[0].load(std::memory_order_acquire) ||
           g_mouseButtonStates[1].load(std::memory_order_acquire) ||
           g_mouseButtonStates[2].load(std::memory_order_acquire);
}

void ResetMouseState()
{
    for (auto& state : g_mouseButtonStates) state.store(false, std::memory_order_release);
    g_pointerDragOwnedByUI.store(false, std::memory_order_release);
}

bool IsCursorOverContent(const Core::PrismaViewId& viewId, int x, int y)
{
    if (viewId == 0 || !g_viewsMap || !g_viewsMapMutex) return true;

    std::shared_ptr<Core::PrismaView> viewData;
    {
        std::shared_lock lock(*g_viewsMapMutex);
        auto it = g_viewsMap->find(viewId);
        if (it != g_viewsMap->end()) viewData = it->second;
    }

    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) return false;

    std::lock_guard lock(viewData->bufferMutex);
    if (viewData->pixelBuffer.empty() || viewData->bufferWidth == 0 || viewData->bufferHeight == 0 ||
        viewData->bufferStride == 0) {
        return true;
    }
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= viewData->bufferWidth ||
        static_cast<uint32_t>(y) >= viewData->bufferHeight) {
        return false;
    }

    const size_t offset = static_cast<size_t>(y) * viewData->bufferStride + static_cast<size_t>(x) * 4u + 3u;
    if (offset >= viewData->pixelBuffer.size()) return true;
    return std::to_integer<std::uint8_t>(viewData->pixelBuffer[offset]) >= 12;
}

std::string BuildImeFocusTrackingScript()
{
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

void InstallImeFocusTrackingForView(const Core::PrismaViewId& viewId)
{
    auto* executor = g_ultralightThreadExecutor;
    auto* viewsMap = g_viewsMap;
    auto* viewsMutex = g_viewsMapMutex;
    if (!executor || !viewsMap || !viewsMutex || viewId == 0) return;

    auto install = [viewId, viewsMap, viewsMutex]() {
        std::shared_ptr<Core::PrismaView> viewData;
        {
            std::shared_lock lock(*viewsMutex);
            auto it = viewsMap->find(viewId);
            if (it != viewsMap->end()) viewData = it->second;
        }

        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView ||
            !viewData->isLoadingFinished.load()) {
            return;
        }

        Communication::BindJSCallbacks(viewId);
        try {
            viewData->ultralightView->EvaluateScript(ultralight::String(BuildImeFocusTrackingScript().c_str()), nullptr);
        } catch (const std::exception& e) {
            logger::error("Failed to install IME tracking for View [{}]: {}", viewId, e.what());
        } catch (...) {
            logger::error("Failed to install IME tracking for View [{}]", viewId);
        }
    };

    if (executor->IsWorkerThread()) install();
    else executor->submit_with_priority(SingleThreadExecutor::Priority::HIGH, install);
}

std::string EscapeForJS(const std::string& text)
{
    std::string escaped;
    try {
        escaped.reserve(text.size() * 2);
    } catch (...) {
        return {};
    }

    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80) {
            if (i + 2 < text.size() && c == 0xE2 && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                (static_cast<unsigned char>(text[i + 2]) == 0xA8 ||
                 static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
                escaped += "\\u202";
                escaped += text[i + 2] == static_cast<char>(0xA8) ? '8' : '9';
                i += 2;
                continue;
            }
            escaped += static_cast<char>(c);
            continue;
        }

        switch (c) {
            case '\'': escaped += "\\'"; break;
            case '\"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            default:
                if (c >= 0x20) escaped += static_cast<char>(c);
                break;
        }
    }
    return escaped;
}

std::string GetClipboardText()
{
    if (!OpenClipboard(g_hWnd)) return {};

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return {};
    }

    const SIZE_T dataSize = GlobalSize(data);
    if (dataSize == 0 || dataSize > MAX_CLIPBOARD_SIZE || dataSize % sizeof(wchar_t) != 0) {
        CloseClipboard();
        return {};
    }

    auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) {
        CloseClipboard();
        return {};
    }

    const size_t maxChars = dataSize / sizeof(wchar_t);
    const wchar_t* end = text + maxChars;
    const wchar_t* terminator = std::find(text, end, L'\0');
    if (terminator == end) {
        GlobalUnlock(data);
        CloseClipboard();
        logger::warn("Clipboard CF_UNICODETEXT was not terminated within its allocation");
        return {};
    }

    const size_t charCount = static_cast<size_t>(terminator - text);
    if (charCount > MAX_CLIPBOARD_CHARS) {
        GlobalUnlock(data);
        CloseClipboard();
        return {};
    }

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(charCount), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        GlobalUnlock(data);
        CloseClipboard();
        return {};
    }

    std::string result(static_cast<size_t>(utf8Length), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(charCount), result.data(), utf8Length,
                                               nullptr, nullptr);
    GlobalUnlock(data);
    CloseClipboard();
    if (converted != utf8Length) return {};
    return result;
}

void SetClipboardText(const std::string& text)
{
    if (text.size() > MAX_CLIPBOARD_SIZE || !OpenClipboard(g_hWnd)) return;

    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wideLength <= 0) {
        CloseClipboard();
        return;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wideLength) * sizeof(wchar_t));
    if (!memory) {
        CloseClipboard();
        return;
    }

    auto* buffer = static_cast<wchar_t*>(GlobalLock(memory));
    if (!buffer) {
        GlobalFree(memory);
        CloseClipboard();
        return;
    }

    if (MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer, wideLength) <= 0) {
        GlobalUnlock(memory);
        GlobalFree(memory);
        CloseClipboard();
        return;
    }
    GlobalUnlock(memory);

    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

bool IsHighSurrogate(wchar_t ch) { return ch >= 0xD800 && ch <= 0xDBFF; }
bool IsLowSurrogate(wchar_t ch) { return ch >= 0xDC00 && ch <= 0xDFFF; }
bool ShouldQueueChar(wchar_t ch) { return ch >= 0x20 || ch == L'\t'; }

std::string ConvertUtf16ToUtf8(const wchar_t* text, int length)
{
    if (!text || length <= 0) return {};
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return {};

    std::string result(static_cast<size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), utf8Length, nullptr, nullptr) != utf8Length) {
        return {};
    }
    return result;
}

void QueueCommittedCharEvent(const std::wstring& utf16Text, LPARAM lParam)
{
    if (utf16Text.empty()) return;
    const auto capture = SnapshotCapture();
    if (capture.viewId == 0 || !g_isAnyInputCaptureActive.load(std::memory_order_acquire)) return;

    const std::string utf8Text = ConvertUtf16ToUtf8(utf16Text.data(), static_cast<int>(utf16Text.size()));
    if (utf8Text.empty()) return;

    ultralight::KeyEvent event;
    event.type = ultralight::KeyEvent::kType_Char;
    WinKeyHandler::GetUltralightModifiers(event);
    const ultralight::String text(utf8Text.c_str());
    event.text = text;
    event.unmodified_text = text;
    event.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
    event.native_key_code = 0;
    event.key_identifier = "";
    event.is_keypad = false;
    event.is_auto_repeat = (HIWORD(lParam) & KF_REPEAT) == KF_REPEAT;
    event.is_system_key = false;
    QueueEvent(capture.generation, event);
}

std::wstring ConvertCodePointToUtf16(UINT codePoint)
{
    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) return L"";
    if (codePoint <= 0xFFFF) return std::wstring(1, static_cast<wchar_t>(codePoint));

    codePoint -= 0x10000;
    const wchar_t high = static_cast<wchar_t>(0xD800 + (codePoint >> 10));
    const wchar_t low = static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF));
    return std::wstring{high, low};
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                              DWORD_PTR)
{
    if (GameThreadDispatcher::HandleWindowMessage(hwnd, uMsg)) return 0;

    LRESULT imeControlResult = 0;
    if (g_imeHelper.HandleControlMessage(hwnd, uMsg, wParam, lParam, &imeControlResult)) return imeControlResult;

    if (uMsg == WM_IME_SETCONTEXT) {
        LPARAM imeLParam = lParam;
        g_imeHelper.ModifySetContextLParam(&imeLParam, uMsg);
        lParam = imeLParam;
    }

    if (uMsg == WM_MOUSEMOVE) {
        g_lastCursorX.store(GET_X_LPARAM(lParam), std::memory_order_release);
        g_lastCursorY.store(GET_Y_LPARAM(lParam), std::memory_order_release);
    }

    if (uMsg == WM_LBUTTONDOWN) {
        std::vector<OverlayClickCallback> handlers;
        {
            std::lock_guard lock(g_overlayClickMutex);
            handlers = g_overlayClickHandlers;
        }
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        for (auto& callback : handlers) {
            if (callback && callback(x, y)) return 0;
        }
    }

    if (g_isAnyInputCaptureActive.load(std::memory_order_acquire)) {
        if (uMsg == WM_SETCURSOR) {
            SetCursor(nullptr);
            return TRUE;
        }

        bool handledByUI = false;
        const auto capture = SnapshotCapture();
        const Core::PrismaViewId focusedViewId = capture.viewId;

        if (focusedViewId != 0) {
            if (g_imeHelper.HandleMessage(hwnd, uMsg, wParam, lParam, focusedViewId, &handledByUI) && handledByUI) {
                return 0;
            }

            switch (uMsg) {
                case WM_KEYDOWN: {
                    if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'V') {
                        if (g_isFocusedTextInputActive.load(std::memory_order_acquire)) {
                            const std::string clipboardText = GetClipboardText();
                            if (!clipboardText.empty()) {
                                const std::string escapedText = EscapeForJS(clipboardText);
                                if (!escapedText.empty()) {
                                    Communication::Invoke(focusedViewId,
                                        ultralight::String(("document.execCommand('insertText', false, '" +
                                                           escapedText + "')").c_str()));
                                }
                            }
                        }
                        handledByUI = true;
                        break;
                    }

                    if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C') {
                        auto* executor = g_ultralightThreadExecutor;
                        auto* viewsMap = g_viewsMap;
                        auto* viewsMutex = g_viewsMapMutex;
                        if (executor && viewsMap && viewsMutex) {
                            executor->submit_with_priority(SingleThreadExecutor::Priority::HIGH,
                                [viewId = focusedViewId, viewsMap, viewsMutex]() {
                                    std::shared_ptr<Core::PrismaView> viewData;
                                    {
                                        std::shared_lock lock(*viewsMutex);
                                        auto it = viewsMap->find(viewId);
                                        if (it != viewsMap->end()) viewData = it->second;
                                    }
                                    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) ||
                                        !viewData->ultralightView) {
                                        return;
                                    }

                                    try {
                                        const ultralight::String result = viewData->ultralightView->EvaluateScript(
                                            ultralight::String("window.getSelection().toString()"), nullptr);
                                        const std::string selectedText = result.utf8().data();
                                        if (selectedText.empty() || selectedText.size() > MAX_CLIPBOARD_SIZE) return;
                                        if (!GameThreadDispatcher::DispatchSafety([text = selectedText]() {
                                                SetClipboardText(text);
                                            })) {
                                            logger::warn("Ctrl+C clipboard dispatch failed");
                                        }
                                    } catch (const std::exception& e) {
                                        logger::error("Ctrl+C failed: {}", e.what());
                                    } catch (...) {
                                        logger::error("Ctrl+C failed");
                                    }
                                });
                        }
                        handledByUI = true;
                        break;
                    }

                    QueueEvent(capture.generation,
                               WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_RawKeyDown, wParam, lParam));
                    handledByUI = true;
                    break;
                }
                case WM_KEYUP:
                    QueueEvent(capture.generation,
                               WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_KeyUp, wParam, lParam));
                    handledByUI = true;
                    break;
                case WM_CHAR: {
                    if (!g_isFocusedTextInputActive.load(std::memory_order_acquire)) {
                        g_pendingHighSurrogate = 0;
                        break;
                    }
                    handledByUI = true;
                    const wchar_t ch = static_cast<wchar_t>(wParam);
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
                        if (ShouldQueueChar(ch)) committedText.push_back(ch);
                    }
                    QueueCommittedCharEvent(committedText, lParam);
                    break;
                }
                case WM_UNICHAR: {
                    if (wParam == UNICODE_NOCHAR) return TRUE;
                    if (!g_isFocusedTextInputActive.load(std::memory_order_acquire)) break;
                    handledByUI = true;
                    const std::wstring committedText = ConvertCodePointToUtf16(static_cast<UINT>(wParam));
                    if (!committedText.empty() && ShouldQueueChar(committedText[0])) {
                        g_pendingHighSurrogate = 0;
                        QueueCommittedCharEvent(committedText, lParam);
                    }
                    break;
                }
                case WM_MOUSEMOVE: {
                    ultralight::MouseEvent event;
                    event.type = ultralight::MouseEvent::kType_MouseMoved;
                    event.x = GET_X_LPARAM(lParam);
                    event.y = GET_Y_LPARAM(lParam);
                    event.button = ultralight::MouseEvent::kButton_None;
                    QueueEvent(capture.generation, event);
                    break;
                }
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN: {
                    const size_t index = uMsg == WM_LBUTTONDOWN ? 0 : (uMsg == WM_RBUTTONDOWN ? 1 : 2);
                    const bool wasAnyDown = AnyMouseButtonDown();
                    g_mouseButtonStates[index].store(true, std::memory_order_release);
                    const int x = GET_X_LPARAM(lParam);
                    const int y = GET_Y_LPARAM(lParam);
                    if (!wasAnyDown) {
                        g_pointerDragOwnedByUI.store(IsCursorOverContent(focusedViewId, x, y), std::memory_order_release);
                    }
                    if (!g_pointerDragOwnedByUI.load(std::memory_order_acquire)) break;

                    ultralight::MouseEvent event;
                    event.type = ultralight::MouseEvent::kType_MouseDown;
                    event.x = x;
                    event.y = y;
                    event.button = index == 0 ? ultralight::MouseEvent::kButton_Left
                                              : (index == 1 ? ultralight::MouseEvent::kButton_Right
                                                            : ultralight::MouseEvent::kButton_Middle);
                    QueueEvent(capture.generation, event);
                    handledByUI = true;
                    break;
                }
                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                case WM_MBUTTONUP: {
                    const size_t index = uMsg == WM_LBUTTONUP ? 0 : (uMsg == WM_RBUTTONUP ? 1 : 2);
                    g_mouseButtonStates[index].store(false, std::memory_order_release);
                    if (g_pointerDragOwnedByUI.load(std::memory_order_acquire)) {
                        ultralight::MouseEvent event;
                        event.type = ultralight::MouseEvent::kType_MouseUp;
                        event.x = GET_X_LPARAM(lParam);
                        event.y = GET_Y_LPARAM(lParam);
                        event.button = index == 0 ? ultralight::MouseEvent::kButton_Left
                                                  : (index == 1 ? ultralight::MouseEvent::kButton_Right
                                                                : ultralight::MouseEvent::kButton_Middle);
                        QueueEvent(capture.generation, event);
                        handledByUI = true;
                    }
                    if (!AnyMouseButtonDown()) g_pointerDragOwnedByUI.store(false, std::memory_order_release);
                    break;
                }
                case WM_MOUSEWHEEL: {
                    int scrollPixelSize = 28;
                    auto* viewsMap = g_viewsMap;
                    auto* viewsMutex = g_viewsMapMutex;
                    if (viewsMap && viewsMutex) {
                        std::shared_lock lock(*viewsMutex);
                        auto it = viewsMap->find(focusedViewId);
                        if (it != viewsMap->end() && it->second &&
                            !it->second->isDestroying.load(std::memory_order_acquire)) {
                            scrollPixelSize = it->second->scrollingPixelSize;
                        }
                    }

                    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    const int scrollAmount = (delta > 0 ? 1 : -1) * SCROLL_LINES_PER_WHEEL_DELTA * scrollPixelSize;
                    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ScreenToClient(hwnd, &point);

                    ScrollEventWithPosition event;
                    event.event.type = ultralight::ScrollEvent::kType_ScrollByPixel;
                    event.event.delta_x = 0;
                    event.event.delta_y = scrollAmount;
                    event.mouseX = point.x;
                    event.mouseY = point.y;
                    QueueEvent(capture.generation, event);
                    handledByUI = true;
                    break;
                }
                default:
                    break;
            }
        }

        if (handledByUI) return 0;
    }

    if (uMsg == WM_NCDESTROY) {
        GameThreadDispatcher::DetachWindow(hwnd);
        RemoveWindowSubclass(hwnd, SubclassProc, uIdSubclass);
        if (uIdSubclass == SUBCLASS_ID) g_wndProcInstalled.store(false, std::memory_order_release);
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void FireResetMouseEvent(Core::PrismaViewId viewId,
                         std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                         std::shared_mutex* viewsMutex)
{
    if (!viewsMap || !viewsMutex || viewId == 0) return;

    std::shared_ptr<Core::PrismaView> viewData;
    {
        std::shared_lock lock(*viewsMutex);
        auto it = viewsMap->find(viewId);
        if (it != viewsMap->end()) viewData = it->second;
    }
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) return;

    ultralight::MouseEvent event;
    event.type = ultralight::MouseEvent::kType_MouseMoved;
    event.x = 0;
    event.y = 0;
    event.button = ultralight::MouseEvent::kButton_None;
    viewData->ultralightView->FireMouseEvent(event);
}

}

void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor,
                std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                std::shared_mutex* viewsMapMutex)
{
    g_hWnd = gameHwnd;
    g_ultralightThreadExecutor = coreExecutor;
    g_viewsMap = viewsMap;
    g_viewsMapMutex = viewsMapMutex;
    g_isAnyInputCaptureActive.store(false, std::memory_order_release);
    g_isFocusedTextInputActive.store(false, std::memory_order_release);
    g_captureGeneration.store(1, std::memory_order_release);
    {
        std::lock_guard lock(g_focusedViewIdMutex);
        g_currentlyFocusedViewId = 0;
    }
    ResetMouseState();
    ClearQueuedEvents();

    g_imeHelper.SetCallbacks(
        [](const std::string& value) { return EscapeForJS(value); },
        [](const std::wstring& value, LPARAM lParam) { QueueCommittedCharEvent(value, lParam); },
        [](const wchar_t* value, int length) { return ConvertUtf16ToUtf8(value, length); });
    g_imeHelper.SetContext({g_hWnd, g_viewsMap, g_viewsMapMutex, &g_focusedViewIdMutex,
                            &g_currentlyFocusedViewId, &g_isAnyInputCaptureActive, &g_isFocusedTextInputActive});
    g_imeHelper.SetExecutor(g_ultralightThreadExecutor);
    g_imeHelper.Initialize(g_hWnd);

    logger::info("PrismaUI input initialized on HWND {:p}", static_cast<void*>(g_hWnd));
}

bool InstallWndProcHook()
{
    std::lock_guard lock(g_wndProcMutex);
    if (g_wndProcInstalled.load(std::memory_order_acquire)) return true;
    if (!g_hWnd || !IsWindow(g_hWnd)) return false;

    DWORD processId = 0;
    const DWORD ownerThread = GetWindowThreadProcessId(g_hWnd, &processId);
    const DWORD currentThread = GetCurrentThreadId();
    if (!ownerThread || processId != GetCurrentProcessId() || currentThread != ownerThread) {
        logger::warn("WndProc install refused off the Fallout window thread (owner={}, current={})", ownerThread,
                     currentThread);
        return false;
    }

    SetLastError(0);
    if (!SetWindowSubclass(g_hWnd, SubclassProc, SUBCLASS_ID, 0)) {
        logger::error("SetWindowSubclass failed, GLE={}", GetLastError());
        return false;
    }

    if (!GameThreadDispatcher::AttachWindow(g_hWnd)) {
        RemoveWindowSubclass(g_hWnd, SubclassProc, SUBCLASS_ID);
        logger::error("Game thread dispatcher could not attach to the Fallout window");
        return false;
    }

    g_wndProcInstalled.store(true, std::memory_order_release);
    logger::info("PrismaUI WndProc subclass installed.");
    return true;
}

void UninstallWndProcHook()
{
    std::lock_guard lock(g_wndProcMutex);
    if (!g_wndProcInstalled.exchange(false, std::memory_order_acq_rel)) return;
    if (g_hWnd) {
        GameThreadDispatcher::DetachWindow(g_hWnd);
        RemoveWindowSubclass(g_hWnd, SubclassProc, SUBCLASS_ID);
    }
}

void EnableInputCapture(const Core::PrismaViewId& viewId)
{
    if (viewId == 0) return;

    {
        std::lock_guard lock(g_focusedViewIdMutex);
        if (g_currentlyFocusedViewId != viewId) {
            g_currentlyFocusedViewId = viewId;
            g_captureGeneration.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    g_isAnyInputCaptureActive.store(true, std::memory_order_release);
    g_isFocusedTextInputActive.store(false, std::memory_order_release);
    g_imeHelper.SetAssociation(false);
    ResetMouseState();
    ClearQueuedEvents();

    Communication::RegisterJSListener(viewId, IME_FOCUS_CALLBACK_NAME, [viewId](std::string focused) {
        const auto capture = SnapshotCapture();
        if (capture.viewId != viewId || !g_isAnyInputCaptureActive.load(std::memory_order_acquire)) return;

        const bool textFocused = focused == "1";
        g_isFocusedTextInputActive.store(textFocused, std::memory_order_release);
        g_imeHelper.SetAssociation(textFocused);

        if (!textFocused) {
            auto* executor = g_ultralightThreadExecutor;
            if (executor && !executor->IsWorkerThread()) {
                executor->submit_with_priority(SingleThreadExecutor::Priority::HIGH,
                    [viewId]() { g_imeHelper.ClearStateInJS(viewId); });
            } else {
                g_imeHelper.ClearStateInJS(viewId);
            }
        }
    });
    InstallImeFocusTrackingForView(viewId);
}

void DisableInputCapture(const Core::PrismaViewId& viewIdToUnfocus)
{
    Core::PrismaViewId previous = 0;
    bool disabled = false;
    {
        std::lock_guard lock(g_focusedViewIdMutex);
        previous = g_currentlyFocusedViewId;
        if (viewIdToUnfocus == 0 || viewIdToUnfocus == previous) {
            if (g_isAnyInputCaptureActive.exchange(false, std::memory_order_acq_rel)) {
                g_currentlyFocusedViewId = 0;
                g_captureGeneration.fetch_add(1, std::memory_order_acq_rel);
                disabled = true;
            }
        }
    }

    if (!disabled) return;

    g_isFocusedTextInputActive.store(false, std::memory_order_release);
    g_imeHelper.SetAssociation(false);
    ResetMouseState();
    ClearQueuedEvents();

    auto* executor = g_ultralightThreadExecutor;
    auto* viewsMap = g_viewsMap;
    auto* viewsMutex = g_viewsMapMutex;
    if (!executor || previous == 0) return;

    auto reset = [previous, viewsMap, viewsMutex]() { FireResetMouseEvent(previous, viewsMap, viewsMutex); };
    if (executor->IsWorkerThread()) reset();
    else executor->submit_with_priority(SingleThreadExecutor::Priority::HIGH, reset);
}

void ClearImeState(const Core::PrismaViewId& viewId)
{
    if (viewId == 0) return;
    g_isFocusedTextInputActive.store(false, std::memory_order_release);
    g_imeHelper.SetAssociation(false);
    g_imeHelper.ClearStateInJS(viewId);
}

bool IsAnyInputCaptureActive()
{
    return g_isAnyInputCaptureActive.load(std::memory_order_acquire);
}

Core::PrismaViewId GetFocusedViewId()
{
    return SnapshotCapture().viewId;
}

int GetLastCursorX() { return g_lastCursorX.load(std::memory_order_acquire); }
int GetLastCursorY() { return g_lastCursorY.load(std::memory_order_acquire); }

bool IsInputCaptureActiveForView(const Core::PrismaViewId& viewId)
{
    const auto capture = SnapshotCapture();
    if (viewId == 0) return g_isAnyInputCaptureActive.load(std::memory_order_acquire);
    return g_isAnyInputCaptureActive.load(std::memory_order_acquire) && capture.viewId == viewId;
}

void ProcessEvents()
{
    auto* executor = g_ultralightThreadExecutor;
    auto* viewsMap = g_viewsMap;
    auto* viewsMutex = g_viewsMapMutex;
    if (!executor || !viewsMap || !viewsMutex) return;

    const auto capture = SnapshotCapture();
    if (capture.viewId == 0 || !g_isAnyInputCaptureActive.load(std::memory_order_acquire)) {
        ClearQueuedEvents();
        return;
    }

    std::vector<InputEvent> events;
    {
        std::lock_guard lock(g_eventQueueMutex);
        for (auto& queued : g_eventQueue) {
            if (queued.generation == capture.generation) events.push_back(std::move(queued.event));
        }
        g_eventQueue.clear();
    }
    if (events.empty()) return;

    auto dispatch = [viewId = capture.viewId, generation = capture.generation, events = std::move(events),
                     viewsMap, viewsMutex]() mutable {
        const auto liveCapture = SnapshotCapture();
        if (!g_isAnyInputCaptureActive.load(std::memory_order_acquire) || liveCapture.viewId != viewId ||
            liveCapture.generation != generation) {
            return;
        }

        std::shared_ptr<Core::PrismaView> viewData;
        {
            std::shared_lock lock(*viewsMutex);
            auto it = viewsMap->find(viewId);
            if (it != viewsMap->end()) viewData = it->second;
        }
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) return;

        ultralight::View* view = viewData->ultralightView.get();
        ultralight::View* inspector = viewData->inspectorView ? viewData->inspectorView.get() : nullptr;

        for (const auto& variant : events) {
            std::visit([view, inspector, &viewData](const auto& event) {
                using T = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<T, ultralight::MouseEvent>) {
                    bool overInspector = false;
                    if (inspector && viewData->inspectorVisible.load()) {
                        const float x = static_cast<float>(event.x);
                        const float y = static_cast<float>(event.y);
                        const float left = viewData->inspectorPosX;
                        const float top = viewData->inspectorPosY;
                        const float right = left + static_cast<float>(viewData->inspectorDisplayWidth);
                        const float bottom = top + static_cast<float>(viewData->inspectorDisplayHeight);
                        overInspector = x >= left && x < right && y >= top && y < bottom;
                        viewData->inspectorPointerHover.store(overInspector);
                    }

                    if (overInspector) {
                        ultralight::MouseEvent translated = event;
                        translated.x -= static_cast<int>(viewData->inspectorPosX);
                        translated.y -= static_cast<int>(viewData->inspectorPosY);
                        inspector->FireMouseEvent(translated);
                    } else {
                        view->FireMouseEvent(event);
                    }
                } else if constexpr (std::is_same_v<T, ScrollEventWithPosition>) {
                    bool overInspector = false;
                    if (inspector && viewData->inspectorVisible.load()) {
                        const float x = static_cast<float>(event.mouseX);
                        const float y = static_cast<float>(event.mouseY);
                        const float left = viewData->inspectorPosX;
                        const float top = viewData->inspectorPosY;
                        const float right = left + static_cast<float>(viewData->inspectorDisplayWidth);
                        const float bottom = top + static_cast<float>(viewData->inspectorDisplayHeight);
                        overInspector = x >= left && x < right && y >= top && y < bottom;
                    }
                    if (overInspector) inspector->FireScrollEvent(event.event);
                    else view->FireScrollEvent(event.event);
                } else if constexpr (std::is_same_v<T, ultralight::KeyEvent>) {
                    if (inspector && viewData->inspectorVisible.load() && inspector->HasFocus()) {
                        inspector->FireKeyEvent(event);
                    } else {
                        view->FireKeyEvent(event);
                    }
                }
            }, variant);
        }
    };

    if (executor->IsWorkerThread()) dispatch();
    else executor->submit_with_priority(SingleThreadExecutor::Priority::HIGH, std::move(dispatch));
}

void RegisterOverlayClickHandler(OverlayClickCallback cb)
{
    if (!cb) return;
    std::lock_guard lock(g_overlayClickMutex);
    g_overlayClickHandlers.push_back(std::move(cb));
}

void Shutdown()
{
    DisableInputCapture(0);
    ClearQueuedEvents();
    UninstallWndProcHook();
    g_imeHelper.Shutdown(g_hWnd);

    g_hWnd = nullptr;
    g_ultralightThreadExecutor = nullptr;
    g_viewsMap = nullptr;
    g_viewsMapMutex = nullptr;
    g_isFocusedTextInputActive.store(false, std::memory_order_release);
    ResetMouseState();
}

}
