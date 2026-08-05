#include "PCH.h"

#include "PrismaUI/InputHandler.h"

#include "PrismaUI/Communication.h"
#include "PrismaUI/ImeHelper.h"
#include "PrismaUI/ViewManager.h"

#include <CommCtrl.h>
#include <windowsx.h>

#include <array>

namespace PrismaUI::InputHandler
{
    namespace
    {
        constexpr UINT_PTR kWindowSubclassId =
            0x505249534D415652ull;
        constexpr std::size_t kMaximumClipboardBytes =
            1024u * 1024u;
        constexpr std::size_t kMaximumClipboardCharacters =
            256u * 1024u;

        enum class QueuedKind : std::uint8_t
        {
            KeyDown,
            KeyUp,
            Character,
            MouseMove,
            MouseDown,
            MouseUp,
            Scroll,
            Copy,
            Paste
        };

        struct QueuedEvent
        {
            Core::PrismaViewId viewId = 0;
            QueuedKind kind = QueuedKind::MouseMove;
            WPARAM wParam = 0;
            LPARAM lParam = 0;
            int x = -1;
            int y = -1;
            int deltaX = 0;
            int deltaY = 0;
            std::uint32_t modifiers = 0;
            std::array<wchar_t, 2> characters{};
            std::uint8_t characterCount = 0;
            ultralight::MouseEvent::Button button =
                ultralight::MouseEvent::kButton_None;
            bool spatial = false;
            bool forced = false;
            bool buttonDown = false;
        };

        struct Queue
        {
            std::array<QueuedEvent, kInputEventCapacity> events{};
            std::size_t head = 0;
            std::size_t count = 0;
            std::mutex mutex;
        };

        Queue g_queue;
        ImeHelper g_ime;

        std::atomic<HWND> g_window = nullptr;
        std::atomic<bool> g_hookInstalled = false;
        std::atomic<bool> g_captureActive = false;
        std::atomic<Core::PrismaViewId> g_focusedView = 0;
        std::mutex g_focusTransitionMutex;
        std::atomic<bool> g_suppressCursor = false;
        std::atomic<std::uint32_t> g_logicalWidth = 0;
        std::atomic<std::uint32_t> g_logicalHeight = 0;
        std::atomic<int> g_cursorX = 0;
        std::atomic<int> g_cursorY = 0;
        std::atomic<bool> g_leftButtonDown = false;
        std::atomic<std::uint32_t> g_overflowLogCount = 0;
        std::atomic<wchar_t> g_pendingHighSurrogate = 0;

        [[nodiscard]] bool IsHighSurrogate(wchar_t value) noexcept
        {
            return value >= 0xD800 && value <= 0xDBFF;
        }

        [[nodiscard]] bool IsLowSurrogate(wchar_t value) noexcept
        {
            return value >= 0xDC00 && value <= 0xDFFF;
        }

        [[nodiscard]] std::uint32_t CurrentModifiers() noexcept
        {
            std::uint32_t modifiers = 0;
            if (GetKeyState(VK_MENU) < 0) {
                modifiers |= ultralight::KeyEvent::kMod_AltKey;
            }
            if (GetKeyState(VK_CONTROL) < 0) {
                modifiers |= ultralight::KeyEvent::kMod_CtrlKey;
            }
            if (GetKeyState(VK_SHIFT) < 0) {
                modifiers |= ultralight::KeyEvent::kMod_ShiftKey;
            }
            if (GetKeyState(VK_LWIN) < 0 ||
                GetKeyState(VK_RWIN) < 0) {
                modifiers |= ultralight::KeyEvent::kMod_MetaKey;
            }
            return modifiers;
        }

        void LogQueueOverflow() noexcept
        {
            const auto count = g_overflowLogCount.fetch_add(
                1,
                std::memory_order_relaxed);
            if (count < 4 || count % 256 == 0) {
                logger::warn(
                    "PrismaUI input queue is full; dropping an event");
            }
        }

        [[nodiscard]] bool PushEvent(
            const QueuedEvent& event) noexcept
        {
            std::lock_guard lock(g_queue.mutex);
            if (g_queue.count == g_queue.events.size()) {
                LogQueueOverflow();
                return false;
            }
            const auto tail =
                (g_queue.head + g_queue.count) %
                g_queue.events.size();
            g_queue.events[tail] = event;
            ++g_queue.count;
            return true;
        }

        void RemoveEventsForView(
            Core::PrismaViewId viewId) noexcept
        {
            std::lock_guard lock(g_queue.mutex);
            std::size_t retained = 0;
            for (std::size_t index = 0;
                 index < g_queue.count;
                 ++index) {
                const auto source =
                    (g_queue.head + index) %
                    g_queue.events.size();
                if (g_queue.events[source].viewId == viewId) {
                    continue;
                }
                const auto destination =
                    (g_queue.head + retained) %
                    g_queue.events.size();
                if (source != destination) {
                    g_queue.events[destination] =
                        g_queue.events[source];
                }
                ++retained;
            }
            g_queue.count = retained;
        }

        [[nodiscard]] std::size_t DrainEvents(
            std::span<QueuedEvent> output) noexcept
        {
            std::lock_guard lock(g_queue.mutex);
            const auto count = (std::min)(
                output.size(),
                g_queue.count);
            for (std::size_t index = 0; index < count; ++index) {
                output[index] =
                    g_queue.events[
                        (g_queue.head + index) %
                        g_queue.events.size()];
            }
            g_queue.head =
                (g_queue.head + count) %
                g_queue.events.size();
            g_queue.count -= count;
            if (g_queue.count == 0) {
                g_queue.head = 0;
            }
            return count;
        }

        [[nodiscard]] std::string ToUtf8(
            std::wstring_view value)
        {
            if (value.empty() ||
                value.size() >
                    static_cast<std::size_t>(
                        (std::numeric_limits<int>::max)())) {
                return {};
            }
            const auto required = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (required <= 0) {
                return {};
            }
            std::string result(
                static_cast<std::size_t>(required),
                '\0');
            if (WideCharToMultiByte(
                    CP_UTF8,
                    WC_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required,
                    nullptr,
                    nullptr) != required) {
                return {};
            }
            return result;
        }

        [[nodiscard]] std::wstring ToUtf16(
            std::string_view value)
        {
            if (value.empty() ||
                value.size() >
                    static_cast<std::size_t>(
                        (std::numeric_limits<int>::max)())) {
                return {};
            }
            const auto required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
            if (required <= 0) {
                return {};
            }
            std::wstring result(
                static_cast<std::size_t>(required),
                L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    required) != required) {
                return {};
            }
            return result;
        }

        [[nodiscard]] std::string ReadClipboard()
        {
            const auto window = g_window.load(
                std::memory_order_acquire);
            if (!OpenClipboard(window)) {
                return {};
            }

            struct ClipboardGuard
            {
                ~ClipboardGuard() { CloseClipboard(); }
            } guard;

            const auto handle = GetClipboardData(CF_UNICODETEXT);
            if (!handle) {
                return {};
            }
            const auto byteSize = GlobalSize(handle);
            if (byteSize == 0 ||
                byteSize > kMaximumClipboardBytes) {
                return {};
            }

            const auto data = static_cast<const wchar_t*>(
                GlobalLock(handle));
            if (!data) {
                return {};
            }
            struct GlobalUnlockGuard
            {
                HGLOBAL handle;
                ~GlobalUnlockGuard() { GlobalUnlock(handle); }
            } unlock{handle};

            const auto capacity = byteSize / sizeof(wchar_t);
            std::size_t length = 0;
            while (length < capacity &&
                   length < kMaximumClipboardCharacters &&
                   data[length] != L'\0') {
                ++length;
            }
            if (length == capacity) {
                return {};
            }
            return ToUtf8(std::wstring_view(data, length));
        }

        void WriteClipboard(std::string_view text) noexcept
        {
            if (text.empty() ||
                text.size() > kMaximumClipboardBytes) {
                return;
            }
            const auto converted = ToUtf16(text);
            if (converted.empty() ||
                converted.size() >
                    kMaximumClipboardCharacters) {
                return;
            }

            const auto window = g_window.load(
                std::memory_order_acquire);
            if (!OpenClipboard(window)) {
                return;
            }
            struct ClipboardGuard
            {
                ~ClipboardGuard() { CloseClipboard(); }
            } guard;
            if (!EmptyClipboard()) {
                return;
            }

            const auto bytes =
                (converted.size() + 1) * sizeof(wchar_t);
            auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!memory) {
                return;
            }
            const auto destination = GlobalLock(memory);
            if (!destination) {
                GlobalFree(memory);
                return;
            }
            std::memcpy(
                destination,
                converted.c_str(),
                bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) {
                GlobalFree(memory);
            }
        }

        [[nodiscard]] bool QueueCharacter(
            Core::PrismaViewId viewId,
            std::wstring_view text,
            LPARAM lParam,
            bool forced = false) noexcept
        {
            if (text.empty() || text.size() > 2) {
                return false;
            }
            QueuedEvent event;
            event.viewId = viewId;
            event.kind = QueuedKind::Character;
            event.lParam = lParam;
            event.modifiers = CurrentModifiers();
            event.characterCount =
                static_cast<std::uint8_t>(text.size());
            std::copy(
                text.begin(),
                text.end(),
                event.characters.begin());
            event.forced = forced;
            return PushEvent(event);
        }

        void QueueCommittedImeText(
            Core::PrismaViewId viewId,
            std::wstring_view text) noexcept
        {
            std::size_t offset = 0;
            while (offset < text.size()) {
                std::size_t count = 1;
                if (IsHighSurrogate(text[offset]) &&
                    offset + 1 < text.size() &&
                    IsLowSurrogate(text[offset + 1])) {
                    count = 2;
                }
                (void)QueueCharacter(
                    viewId,
                    text.substr(offset, count),
                    0);
                offset += count;
            }
        }

        [[nodiscard]] CursorPosition ScaleClientPoint(
            HWND window,
            POINT point) noexcept
        {
            RECT client{};
            if (!GetClientRect(window, &client)) {
                return {point.x, point.y};
            }

            const auto clientWidth =
                (std::max)(client.right - client.left, 1L);
            const auto clientHeight =
                (std::max)(client.bottom - client.top, 1L);
            const auto logicalWidth = g_logicalWidth.load(
                std::memory_order_acquire);
            const auto logicalHeight = g_logicalHeight.load(
                std::memory_order_acquire);
            if (logicalWidth == 0 || logicalHeight == 0) {
                return {point.x, point.y};
            }

            return {
                static_cast<int>(std::lround(
                    static_cast<double>(point.x) *
                    logicalWidth /
                    clientWidth)),
                static_cast<int>(std::lround(
                    static_cast<double>(point.y) *
                    logicalHeight /
                    clientHeight))
            };
        }

        [[nodiscard]] ultralight::View* MouseTarget(
            const std::shared_ptr<Core::PrismaView>& view,
            int& x,
            int& y,
            bool spatial) noexcept
        {
            if (!view || !view->ultralightView) {
                return nullptr;
            }
            if (spatial ||
                !view->inspectorVisible.load(
                    std::memory_order_acquire) ||
                !view->inspectorView) {
                view->inspectorPointerHover.store(
                    false,
                    std::memory_order_release);
                return view->ultralightView.get();
            }

            Core::InspectorPresentationState presentation;
            {
                std::lock_guard lock(
                    view->inspectorPresentationMutex);
                presentation = view->inspectorPresentation;
            }
            const auto left =
                static_cast<int>(std::floor(presentation.x));
            const auto top =
                static_cast<int>(std::floor(presentation.y));
            const auto right =
                left + static_cast<int>(presentation.width);
            const auto bottom =
                top + static_cast<int>(presentation.height);
            const auto inside =
                x >= left && x < right &&
                y >= top && y < bottom;
            view->inspectorPointerHover.store(
                inside,
                std::memory_order_release);
            if (!inside) {
                return view->ultralightView.get();
            }
            x -= left;
            y -= top;
            return view->inspectorView.get();
        }

        [[nodiscard]] ultralight::View* KeyboardTarget(
            const std::shared_ptr<Core::PrismaView>& view) noexcept
        {
            if (!view) {
                return nullptr;
            }
            if (view->inspectorVisible.load(
                    std::memory_order_acquire) &&
                view->inspectorPointerHover.load(
                    std::memory_order_acquire) &&
                view->inspectorView) {
                return view->inspectorView.get();
            }
            return view->ultralightView.get();
        }

        void FireCharacter(
            ultralight::View* target,
            const QueuedEvent& queued,
            std::wstring_view text) noexcept
        {
            if (!target || text.empty()) {
                return;
            }
            const auto utf8 = ToUtf8(text);
            if (utf8.empty()) {
                return;
            }

            ultralight::KeyEvent event;
            event.type = ultralight::KeyEvent::kType_Char;
            event.modifiers = queued.modifiers;
            event.virtual_key_code = 0;
            event.native_key_code = static_cast<int>(
                queued.lParam);
            event.key_identifier = ultralight::String();
            event.text = ultralight::String(utf8.c_str());
            event.unmodified_text =
                ultralight::String(utf8.c_str());
            event.is_keypad = false;
            event.is_auto_repeat =
                (queued.lParam & (1ll << 30)) != 0;
            event.is_system_key = false;
            target->FireKeyEvent(event);
        }

        void Deliver(const QueuedEvent& event) noexcept
        {
            const auto view = Core::FindView(event.viewId);
            if (!view || !view->ultralightView) {
                return;
            }
            if ((!event.spatial &&
                 g_focusedView.load(
                     std::memory_order_acquire) != event.viewId) ||
                (view->destroying.load(
                     std::memory_order_acquire) &&
                 !event.forced) ||
                (view->hidden.load(
                     std::memory_order_acquire) &&
                 !event.forced)) {
                return;
            }

            switch (event.kind) {
            case QueuedKind::KeyDown:
            case QueuedKind::KeyUp: {
                const auto target = KeyboardTarget(view);
                if (!target) {
                    return;
                }
                const auto type =
                    event.kind == QueuedKind::KeyDown ?
                        ultralight::KeyEvent::kType_RawKeyDown :
                        ultralight::KeyEvent::kType_KeyUp;
                const auto alt =
                    (event.modifiers &
                     ultralight::KeyEvent::kMod_AltKey) != 0;
                const auto systemKey =
                    alt &&
                    (event.wParam == VK_TAB ||
                     event.wParam == VK_ESCAPE ||
                     event.wParam == VK_RETURN ||
                     event.wParam == VK_SPACE ||
                     (event.wParam >= VK_F1 &&
                      event.wParam <= VK_F24));
                ultralight::KeyEvent key(
                    type,
                    static_cast<std::uintptr_t>(event.wParam),
                    static_cast<std::intptr_t>(event.lParam),
                    systemKey);
                key.modifiers = event.modifiers;
                target->FireKeyEvent(key);
                break;
            }

            case QueuedKind::Character:
                FireCharacter(
                    KeyboardTarget(view),
                    event,
                    std::wstring_view(
                        event.characters.data(),
                        event.characterCount));
                break;

            case QueuedKind::MouseMove:
            case QueuedKind::MouseDown:
            case QueuedKind::MouseUp: {
                auto x = event.x;
                auto y = event.y;
                const auto target = MouseTarget(
                    view,
                    x,
                    y,
                    event.spatial);
                if (!target) {
                    return;
                }

                ultralight::MouseEvent mouse;
                mouse.type =
                    event.kind == QueuedKind::MouseMove ?
                        ultralight::MouseEvent::kType_MouseMoved :
                    event.kind == QueuedKind::MouseDown ?
                        ultralight::MouseEvent::kType_MouseDown :
                        ultralight::MouseEvent::kType_MouseUp;
                mouse.x = x;
                mouse.y = y;
                mouse.button =
                    event.kind == QueuedKind::MouseMove ?
                        (event.buttonDown ?
                             ultralight::MouseEvent::kButton_Left :
                             ultralight::MouseEvent::kButton_None) :
                        event.button;
                target->FireMouseEvent(mouse);
                break;
            }

            case QueuedKind::Scroll: {
                auto x = event.x;
                auto y = event.y;
                const auto target = MouseTarget(
                    view,
                    x,
                    y,
                    event.spatial);
                if (!target) {
                    return;
                }

                auto deltaX = event.deltaX;
                auto deltaY = event.deltaY;
                if (!event.spatial) {
                    const auto pixels =
                        view->scrollingPixelSize.load(
                            std::memory_order_acquire);
                    deltaX = deltaX * pixels / WHEEL_DELTA;
                    deltaY = deltaY * pixels / WHEEL_DELTA;
                }
                ultralight::ScrollEvent scroll;
                scroll.type =
                    ultralight::ScrollEvent::kType_ScrollByPixel;
                scroll.delta_x = deltaX;
                scroll.delta_y = deltaY;
                target->FireScrollEvent(scroll);
                break;
            }

            case QueuedKind::Copy: {
                const auto target = KeyboardTarget(view);
                if (!target) {
                    return;
                }
                try {
                    const auto selected = target->EvaluateScript(
                        ultralight::String(
                            "(function(){"
                            "var s=window.getSelection?"
                            "window.getSelection():null;"
                            "if(s&&String(s).length){return String(s);}"
                            "var e=document.activeElement;"
                            "return e&&typeof e.value!=='undefined'?"
                            "String(e.value):'';"
                            "})()"),
                        nullptr);
                    const auto& utf8 = selected.utf8();
                    if (utf8.length() <= kMaximumClipboardBytes) {
                        WriteClipboard(std::string_view(
                            utf8.data(),
                            utf8.length()));
                    }
                } catch (...) {
                    logger::warn(
                        "PrismaUI could not copy the current web selection");
                }
                break;
            }

            case QueuedKind::Paste: {
                const auto text = ReadClipboard();
                if (text.empty()) {
                    return;
                }
                const auto utf16 = ToUtf16(text);
                std::size_t offset = 0;
                while (offset < utf16.size()) {
                    auto count = std::size_t{1};
                    if (IsHighSurrogate(utf16[offset]) &&
                        offset + 1 < utf16.size() &&
                        IsLowSurrogate(utf16[offset + 1])) {
                        count = 2;
                    }
                    FireCharacter(
                        KeyboardTarget(view),
                        event,
                        std::wstring_view(
                            utf16.data() + offset,
                            count));
                    offset += count;
                }
                break;
            }
            }
        }

        [[nodiscard]] bool IsCapturedMessage(UINT message) noexcept
        {
            switch (message) {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR:
            case WM_UNICHAR:
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return true;
            default:
                return false;
            }
        }

        LRESULT CALLBACK WindowSubclass(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam,
            UINT_PTR subclassId,
            [[maybe_unused]] DWORD_PTR referenceData)
        {
            LRESULT controlResult = 0;
            if (g_ime.HandleControlMessage(
                    window,
                    message,
                    wParam,
                    lParam,
                    controlResult)) {
                return controlResult;
            }

            g_ime.SuppressNativeUi(message, lParam);

            if (message == WM_NCDESTROY) {
                RemoveWindowSubclass(
                    window,
                    WindowSubclass,
                    subclassId);
                g_hookInstalled.store(
                    false,
                    std::memory_order_release);
                g_window.store(nullptr, std::memory_order_release);
                return DefSubclassProc(
                    window,
                    message,
                    wParam,
                    lParam);
            }

            const auto capture = g_captureActive.load(
                std::memory_order_acquire);
            if (capture &&
                message >= WM_IME_STARTCOMPOSITION &&
                message <= WM_IME_KEYUP &&
                g_ime.HandleImeMessage(
                    window,
                    message,
                    wParam,
                    lParam)) {
                return 0;
            }

            if (message == WM_SETCURSOR &&
                (capture ||
                 g_suppressCursor.exchange(
                     false,
                     std::memory_order_acq_rel))) {
                if (LOWORD(lParam) == HTCLIENT) {
                    SetCursor(nullptr);
                    return TRUE;
                }
            }

            if (!capture) {
                if (message == WM_MOUSEMOVE) {
                    POINT point{
                        GET_X_LPARAM(lParam),
                        GET_Y_LPARAM(lParam)
                    };
                    const auto scaled =
                        ScaleClientPoint(window, point);
                    g_cursorX.store(
                        scaled.x,
                        std::memory_order_release);
                    g_cursorY.store(
                        scaled.y,
                        std::memory_order_release);
                }
                return DefSubclassProc(
                    window,
                    message,
                    wParam,
                    lParam);
            }

            const auto viewId = g_focusedView.load(
                std::memory_order_acquire);
            if (viewId == 0) {
                return DefSubclassProc(
                    window,
                    message,
                    wParam,
                    lParam);
            }

            QueuedEvent queued;
            queued.viewId = viewId;
            queued.wParam = wParam;
            queued.lParam = lParam;
            queued.modifiers = CurrentModifiers();

            switch (message) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if ((queued.modifiers &
                     ultralight::KeyEvent::kMod_CtrlKey) != 0 &&
                    (wParam == 'C' || wParam == 'V')) {
                    queued.kind =
                        wParam == 'C' ?
                            QueuedKind::Copy :
                            QueuedKind::Paste;
                } else {
                    queued.kind = QueuedKind::KeyDown;
                }
                (void)PushEvent(queued);
                break;

            case WM_KEYUP:
            case WM_SYSKEYUP:
                queued.kind = QueuedKind::KeyUp;
                (void)PushEvent(queued);
                break;

            case WM_CHAR: {
                const auto character =
                    static_cast<wchar_t>(wParam);
                if (IsHighSurrogate(character)) {
                    g_pendingHighSurrogate.store(
                        character,
                        std::memory_order_release);
                } else {
                    std::array<wchar_t, 2> text{};
                    std::size_t count = 1;
                    text[0] = character;
                    const auto high =
                        g_pendingHighSurrogate.exchange(
                            0,
                            std::memory_order_acq_rel);
                    if (high != 0 &&
                        IsLowSurrogate(character)) {
                        text[0] = high;
                        text[1] = character;
                        count = 2;
                    }
                    if (character >= 0x20 ||
                        character == L'\t') {
                        (void)QueueCharacter(
                            viewId,
                            std::wstring_view(
                                text.data(),
                                count),
                            lParam);
                    }
                }
                break;
            }

            case WM_UNICHAR:
                if (wParam == UNICODE_NOCHAR) {
                    return TRUE;
                }
                if (wParam <= 0xFFFFu &&
                    !(wParam >= 0xD800u &&
                      wParam <= 0xDFFFu)) {
                    const auto character =
                        static_cast<wchar_t>(wParam);
                    (void)QueueCharacter(
                        viewId,
                        std::wstring_view(&character, 1),
                        lParam);
                } else if (wParam <= 0x10FFFFu) {
                    const auto value =
                        static_cast<std::uint32_t>(
                            wParam - 0x10000u);
                    const std::array<wchar_t, 2> text{
                        static_cast<wchar_t>(
                            0xD800u + (value >> 10u)),
                        static_cast<wchar_t>(
                            0xDC00u + (value & 0x3FFu))
                    };
                    (void)QueueCharacter(
                        viewId,
                        std::wstring_view(text.data(), 2),
                        lParam);
                }
                break;

            case WM_MOUSEMOVE: {
                POINT point{
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)
                };
                const auto scaled =
                    ScaleClientPoint(window, point);
                g_cursorX.store(
                    scaled.x,
                    std::memory_order_release);
                g_cursorY.store(
                    scaled.y,
                    std::memory_order_release);
                queued.kind = QueuedKind::MouseMove;
                queued.x = scaled.x;
                queued.y = scaled.y;
                queued.buttonDown = g_leftButtonDown.load(
                    std::memory_order_acquire);
                (void)PushEvent(queued);
                break;
            }

            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP: {
                POINT point{
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)
                };
                const auto scaled =
                    ScaleClientPoint(window, point);
                const auto down =
                    message == WM_LBUTTONDOWN ||
                    message == WM_RBUTTONDOWN ||
                    message == WM_MBUTTONDOWN;
                queued.kind =
                    down ?
                        QueuedKind::MouseDown :
                        QueuedKind::MouseUp;
                queued.button =
                    message == WM_LBUTTONDOWN ||
                    message == WM_LBUTTONUP ?
                        ultralight::MouseEvent::kButton_Left :
                    message == WM_RBUTTONDOWN ||
                    message == WM_RBUTTONUP ?
                        ultralight::MouseEvent::kButton_Right :
                        ultralight::MouseEvent::kButton_Middle;
                queued.x = scaled.x;
                queued.y = scaled.y;
                if (queued.button ==
                    ultralight::MouseEvent::kButton_Left) {
                    g_leftButtonDown.store(
                        down,
                        std::memory_order_release);
                }
                if (down) {
                    SetCapture(window);
                } else if (
                    GetCapture() == window &&
                    !g_leftButtonDown.load(
                        std::memory_order_acquire)) {
                    ReleaseCapture();
                }
                (void)PushEvent(queued);
                break;
            }

            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                POINT point{
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)
                };
                ScreenToClient(window, &point);
                const auto scaled =
                    ScaleClientPoint(window, point);
                queued.kind = QueuedKind::Scroll;
                queued.x = scaled.x;
                queued.y = scaled.y;
                const auto delta =
                    GET_WHEEL_DELTA_WPARAM(wParam);
                if (message == WM_MOUSEHWHEEL) {
                    queued.deltaX = delta;
                } else {
                    queued.deltaY = delta;
                }
                (void)PushEvent(queued);
                break;
            }

            case WM_KILLFOCUS:
                g_leftButtonDown.store(
                    false,
                    std::memory_order_release);
                break;

            default:
                break;
            }

            if (IsCapturedMessage(message)) {
                return 0;
            }
            return DefSubclassProc(
                window,
                message,
                wParam,
                lParam);
        }
    }

    void Initialize(HWND gameWindow) noexcept
    {
        if (!gameWindow || !IsWindow(gameWindow)) {
            return;
        }
        const auto current = g_window.load(
            std::memory_order_acquire);
        if (current && current != gameWindow) {
            logger::error(
                "PrismaUI input cannot migrate between game windows");
            return;
        }
        g_window.store(gameWindow, std::memory_order_release);
        g_ime.Initialize(gameWindow, QueueCommittedImeText);
    }

    bool InstallWindowHook() noexcept
    {
        if (g_hookInstalled.load(std::memory_order_acquire)) {
            return true;
        }
        const auto window = g_window.load(
            std::memory_order_acquire);
        if (!window || !IsWindow(window)) {
            return false;
        }
        if (!SetWindowSubclass(
                window,
                WindowSubclass,
                kWindowSubclassId,
                0)) {
            logger::error(
                "PrismaUI could not subclass the Fallout 4 VR window");
            return false;
        }
        g_hookInstalled.store(true, std::memory_order_release);
        return true;
    }

    void UninstallWindowHook() noexcept
    {
        if (!g_hookInstalled.exchange(
                false,
                std::memory_order_acq_rel)) {
            return;
        }
        const auto window = g_window.load(
            std::memory_order_acquire);
        if (window && IsWindow(window)) {
            if (!RemoveWindowSubclass(
                    window,
                    WindowSubclass,
                    kWindowSubclassId)) {
                logger::warn(
                    "PrismaUI could not remove its game-window subclass");
            }
        }
    }

    bool EnableInputCapture(
        Core::PrismaViewId viewId) noexcept
    {
        const auto view = Core::FindView(viewId);
        if (!view ||
            view->destroying.load(std::memory_order_acquire) ||
            !view->loadingFinished.load(
                std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard transitionLock(g_focusTransitionMutex);
        const auto previous = g_focusedView.exchange(
            viewId,
            std::memory_order_acq_rel);
        const auto previousView =
            previous != 0 && previous != viewId ?
                Core::FindView(previous) :
                nullptr;
        const auto accepted =
            Core::GetRuntime().worker.TryPost(
                SingleThreadExecutor::Priority::HIGH,
                [view, previousView] {
                    if (previousView &&
                        previousView->ultralightView) {
                        try {
                            previousView->ultralightView->Unfocus();
                        } catch (...) {
                            logger::warn(
                                "View [{}] threw while relinquishing Ultralight focus",
                                previousView->id);
                        }
                    }
                    if (!view->destroying.load(
                            std::memory_order_acquire) &&
                        view->ultralightView) {
                        try {
                            view->ultralightView->Focus();
                        } catch (...) {
                            logger::warn(
                                "View [{}] threw while acquiring Ultralight focus",
                                view->id);
                        }
                    }
                });
        if (!accepted) {
            auto expected = viewId;
            (void)g_focusedView.compare_exchange_strong(
                expected,
                previous,
                std::memory_order_acq_rel);
            logger::warn(
                "View [{}] focus was rejected by the Ultralight worker",
                viewId);
            return false;
        }

        if (previous != 0 && previous != viewId) {
            RemoveEventsForView(previous);
            g_ime.ClearState(previous);
            if (previousView) {
                previousView->focused.store(
                    false,
                    std::memory_order_release);
            }
        }

        view->focused.store(true, std::memory_order_release);
        g_captureActive.store(true, std::memory_order_release);
        g_ime.SetFocusedView(viewId);
        RefreshTextInputTracking(viewId);
        RequestGameCursorSuppression();
        return true;
    }

    void DisableInputCapture(
        Core::PrismaViewId viewId) noexcept
    {
        std::lock_guard transitionLock(g_focusTransitionMutex);
        auto expected = viewId;
        if (!g_focusedView.compare_exchange_strong(
                expected,
                0,
                std::memory_order_acq_rel)) {
            return;
        }

        g_captureActive.store(false, std::memory_order_release);
        g_leftButtonDown.store(false, std::memory_order_release);
        g_pendingHighSurrogate.store(0, std::memory_order_release);
        RemoveEventsForView(viewId);
        g_ime.SetTextInputFocused(false);
        g_ime.SetFocusedView(0);
        g_ime.ClearState(viewId);

        const auto view = Core::FindView(viewId);
        if (view) {
            view->focused.store(false, std::memory_order_release);
            if (!Core::GetRuntime().worker.TryPost(
                SingleThreadExecutor::Priority::HIGH,
                [view] {
                    if (!view->ultralightView) {
                        return;
                    }
                    ultralight::MouseEvent leave;
                    leave.type =
                        ultralight::MouseEvent::kType_MouseMoved;
                    leave.x = -1;
                    leave.y = -1;
                    leave.button =
                        ultralight::MouseEvent::kButton_None;
                    view->ultralightView->FireMouseEvent(leave);
                    view->ultralightView->Unfocus();
                })) {
                logger::warn(
                    "View [{}] unfocus was rejected by the Ultralight worker",
                    viewId);
            }
        }
        if (GetCapture() ==
            g_window.load(std::memory_order_acquire)) {
            ReleaseCapture();
        }
    }

    void ClearImeState(Core::PrismaViewId viewId) noexcept
    {
        g_ime.ClearState(viewId);
    }

    void RefreshTextInputTracking(
        Core::PrismaViewId viewId) noexcept
    {
        if (viewId == 0 ||
            g_focusedView.load(
                std::memory_order_acquire) != viewId) {
            return;
        }

        Communication::RegisterJSListener(
            viewId,
            "__prismaUITextFocus",
            [viewId](std::string state) {
                if (g_focusedView.load(
                        std::memory_order_acquire) != viewId) {
                    return;
                }
                g_ime.SetTextInputFocused(state == "1");
            });

        Communication::Invoke(
            viewId,
            ultralight::String(
                "(()=>{"
                "const n=window.__prismaUITextFocus;"
                "if(typeof n!=='function')return;"
                "const t=e=>!!e&&(e.isContentEditable||"
                "/^(INPUT|TEXTAREA|SELECT)$/.test(e.tagName));"
                "const p=()=>n(t(document.activeElement)?'1':'0');"
                "if(!window.__prismaUITextFocusInstalled){"
                "window.__prismaUITextFocusInstalled=true;"
                "document.addEventListener('focusin',p,true);"
                "document.addEventListener('focusout',()=>"
                "queueMicrotask(p),true);}"
                "p();})()"));
    }

    bool IsInputCaptureActiveForView(
        Core::PrismaViewId viewId) noexcept
    {
        return g_captureActive.load(
                   std::memory_order_acquire) &&
               g_focusedView.load(
                   std::memory_order_acquire) == viewId;
    }

    bool IsAnyInputCaptureActive() noexcept
    {
        return g_captureActive.load(std::memory_order_acquire);
    }

    Core::PrismaViewId GetFocusedViewId() noexcept
    {
        return g_focusedView.load(std::memory_order_acquire);
    }

    void SetLogicalViewportSize(
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        g_logicalWidth.store(width, std::memory_order_release);
        g_logicalHeight.store(height, std::memory_order_release);
    }

    LogicalViewportSize GetLogicalViewportSize() noexcept
    {
        return {
            g_logicalWidth.load(std::memory_order_acquire),
            g_logicalHeight.load(std::memory_order_acquire)
        };
    }

    CursorPosition GetLastCursorPosition() noexcept
    {
        return {
            g_cursorX.load(std::memory_order_acquire),
            g_cursorY.load(std::memory_order_acquire)
        };
    }

    void RequestGameCursorSuppression() noexcept
    {
        g_suppressCursor.store(true, std::memory_order_release);
        const auto window = g_window.load(
            std::memory_order_acquire);
        if (window && IsWindow(window)) {
            POINT point{};
            if (GetCursorPos(&point)) {
                ScreenToClient(window, &point);
                PostMessageW(
                    window,
                    WM_SETCURSOR,
                    reinterpret_cast<WPARAM>(window),
                    MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
        }
    }

    bool EnqueueSpatialPointerEvents(
        std::span<const SpatialPointerEvent> events) noexcept
    {
        if (events.empty()) {
            return true;
        }
        if (events.size() > g_queue.events.size()) {
            return false;
        }
        if (std::any_of(
                events.begin(),
                events.end(),
                [](const SpatialPointerEvent& event) {
                    return event.viewId == 0;
                })) {
            return false;
        }

        std::lock_guard lock(g_queue.mutex);
        if (events.size() >
            g_queue.events.size() - g_queue.count) {
            LogQueueOverflow();
            return false;
        }

        for (const auto& source : events) {
            QueuedEvent queued;
            queued.viewId = source.viewId;
            queued.x = source.x;
            queued.y = source.y;
            queued.deltaX = source.deltaX;
            queued.deltaY = source.deltaY;
            queued.spatial = true;
            queued.forced = source.forced;
            queued.buttonDown = source.buttonDown;
            queued.button =
                ultralight::MouseEvent::kButton_Left;
            switch (source.kind) {
            case SpatialPointerEventKind::Move:
                queued.kind = QueuedKind::MouseMove;
                break;
            case SpatialPointerEventKind::Down:
                queued.kind = QueuedKind::MouseDown;
                break;
            case SpatialPointerEventKind::Up:
                queued.kind = QueuedKind::MouseUp;
                break;
            case SpatialPointerEventKind::Scroll:
                queued.kind = QueuedKind::Scroll;
                break;
            }

            const auto tail =
                (g_queue.head + g_queue.count) %
                g_queue.events.size();
            g_queue.events[tail] = queued;
            ++g_queue.count;
        }
        return true;
    }

    bool ScheduleSpatialPointerEventProcessing() noexcept
    {
        auto& worker = Core::GetRuntime().worker;
        if (worker.IsWorkerThread()) {
            ProcessEvents();
            return true;
        }
        return worker.TryPost(
            SingleThreadExecutor::Priority::FRAME_CRITICAL,
            [] { ProcessEvents(); });
    }

    void FlushSpatialPointerEvents() noexcept
    {
        auto& worker = Core::GetRuntime().worker;
        if (worker.IsWorkerThread()) {
            ProcessEvents();
            return;
        }
        if (!worker.IsStarted()) {
            return;
        }

        try {
            auto completion = worker.submit_with_priority(
                SingleThreadExecutor::Priority::FRAME_CRITICAL,
                [] { ProcessEvents(); });
            if (completion.wait_for(
                    std::chrono::milliseconds(500)) !=
                std::future_status::ready) {
                logger::warn(
                    "PrismaUI timed out flushing spatial pointer events");
                return;
            }
            completion.get();
        } catch (...) {
            logger::warn(
                "PrismaUI could not flush spatial pointer events");
        }
    }

    void ProcessEvents() noexcept
    {
        auto& worker = Core::GetRuntime().worker;
        if (!worker.IsWorkerThread()) {
            (void)worker.TryPost(
                SingleThreadExecutor::Priority::FRAME_CRITICAL,
                [] { ProcessEvents(); });
            return;
        }

        std::array<QueuedEvent, 256> batch{};
        for (;;) {
            const auto count = DrainEvents(batch);
            if (count == 0) {
                return;
            }
            for (std::size_t index = 0;
                 index < count;
                 ++index) {
                try {
                    Deliver(batch[index]);
                } catch (...) {
                    logger::error(
                        "PrismaUI discarded an input event after an Ultralight exception");
                }
            }
            if (count < batch.size()) {
                return;
            }
        }
    }

    void Shutdown() noexcept
    {
        const auto focused = g_focusedView.load(
            std::memory_order_acquire);
        if (focused != 0) {
            DisableInputCapture(focused);
        }
        g_ime.Shutdown();
        UninstallWindowHook();

        {
            std::lock_guard lock(g_queue.mutex);
            g_queue.head = 0;
            g_queue.count = 0;
        }
        g_window.store(nullptr, std::memory_order_release);
        g_logicalWidth.store(0, std::memory_order_release);
        g_logicalHeight.store(0, std::memory_order_release);
        g_suppressCursor.store(false, std::memory_order_release);
    }
}
