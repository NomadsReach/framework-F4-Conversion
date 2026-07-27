#include "PCH.h"

#include "PrismaUI/ImeHelper.h"

#include "PrismaUI/Communication.h"

#include <imm.h>

#include <sstream>

namespace PrismaUI
{
    namespace
    {
        constexpr std::size_t kMaximumCompositionCharacters = 16384;
        constexpr std::size_t kMaximumCandidateBytes =
            1024u * 1024u;
        constexpr std::size_t kMaximumCandidates = 64;

        [[nodiscard]] std::string ToUtf8(
            std::wstring_view value)
        {
            if (value.empty()) {
                return {};
            }
            if (value.size() >
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

        [[nodiscard]] std::string EscapeJson(
            std::string_view value)
        {
            static constexpr char hexadecimal[] =
                "0123456789ABCDEF";

            std::string result;
            result.reserve(value.size() + 16);
            for (std::size_t index = 0;
                 index < value.size();
                 ++index) {
                const auto character =
                    static_cast<unsigned char>(value[index]);
                switch (character) {
                case '\\':
                    result += "\\\\";
                    break;
                case '"':
                    result += "\\\"";
                    break;
                case '\b':
                    result += "\\b";
                    break;
                case '\f':
                    result += "\\f";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    if (character < 0x20u) {
                        result += "\\u00";
                        result.push_back(
                            hexadecimal[(character >> 4u) & 0xFu]);
                        result.push_back(
                            hexadecimal[character & 0xFu]);
                    } else if (
                        index + 2 < value.size() &&
                        character == 0xE2u &&
                        static_cast<unsigned char>(
                            value[index + 1]) == 0x80u &&
                        (static_cast<unsigned char>(
                             value[index + 2]) == 0xA8u ||
                         static_cast<unsigned char>(
                             value[index + 2]) == 0xA9u)) {
                        result +=
                            static_cast<unsigned char>(
                                value[index + 2]) == 0xA8u ?
                                "\\u2028" :
                                "\\u2029";
                        index += 2;
                    } else {
                        result.push_back(
                            static_cast<char>(character));
                    }
                    break;
                }
            }
            return result;
        }
    }

    UINT ImeHelper::AssociationMessage() noexcept
    {
        static const auto message = RegisterWindowMessageW(
            L"PrismaUI_F4VR.ImeAssociation.1");
        return message;
    }

    void ImeHelper::Initialize(
        HWND window,
        CommitTextCallback commitText) noexcept
    {
        if (!window || !IsWindow(window)) {
            return;
        }

        std::lock_guard lock(associationMutex_);
        if (window_) {
            return;
        }

        window_ = window;
        commitText_ = commitText;
        ownedContext_ = ImmCreateContext();
        if (!ownedContext_) {
            logger::warn(
                "PrismaUI could not create a private IME context");
        }
    }

    void ImeHelper::Shutdown() noexcept
    {
        HWND window = nullptr;
        HIMC context = nullptr;
        {
            std::lock_guard lock(associationMutex_);
            window = window_;
            context = ownedContext_;
        }

        bool detached = !associated_.load(
            std::memory_order_acquire);
        if (!detached && window && IsWindow(window)) {
            DWORD windowThread = GetWindowThreadProcessId(
                window,
                nullptr);
            if (windowThread == GetCurrentThreadId()) {
                ApplyAssociationOnWindowThread(false);
                detached = true;
            } else {
                DWORD_PTR ignored = 0;
                detached = SendMessageTimeoutW(
                    window,
                    AssociationMessage(),
                    FALSE,
                    0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    1000,
                    &ignored) != FALSE;
            }
        }

        std::lock_guard lock(associationMutex_);
        if (detached && context) {
            ImmDestroyContext(context);
        } else if (context) {
            logger::warn(
                "PrismaUI retained its IME context because the game window did not acknowledge detachment");
        }
        window_ = nullptr;
        ownedContext_ = nullptr;
        previousContext_ = nullptr;
        commitText_ = nullptr;
        focusedView_.store(0, std::memory_order_release);
        textInputFocused_.store(false, std::memory_order_release);
        associated_.store(false, std::memory_order_release);
    }

    void ImeHelper::SetFocusedView(
        Core::PrismaViewId viewId) noexcept
    {
        focusedView_.store(viewId, std::memory_order_release);
    }

    void ImeHelper::SetTextInputFocused(bool focused) noexcept
    {
        textInputFocused_.store(
            focused,
            std::memory_order_release);
        SetAssociation(
            focused &&
            focusedView_.load(std::memory_order_acquire) != 0);
        if (!focused) {
            PublishState(false);
        }
    }

    void ImeHelper::SetAssociation(bool enabled) noexcept
    {
        HWND window = nullptr;
        {
            std::lock_guard lock(associationMutex_);
            window = window_;
            if (!ownedContext_) {
                enabled = false;
            }
        }
        if (!window || !IsWindow(window)) {
            return;
        }

        if (GetWindowThreadProcessId(window, nullptr) ==
            GetCurrentThreadId()) {
            ApplyAssociationOnWindowThread(enabled);
            return;
        }

        if (!PostMessageW(
                window,
                AssociationMessage(),
                enabled ? TRUE : FALSE,
                0)) {
            logger::warn(
                "PrismaUI could not queue an IME association change");
        }
    }

    void ImeHelper::ClearState(
        Core::PrismaViewId viewId) noexcept
    {
        if (viewId == 0) {
            return;
        }
        Communication::Invoke(
            viewId,
            ultralight::String(
                "(()=>{const d={active:false,composition:'',"
                "candidates:[],selection:0,caret:0};"
                "window.dispatchEvent(new CustomEvent("
                "'prismaui-ime-state',{detail:d}));"
                "if(typeof window.__prismaUIReceiveIme==='function')"
                "window.__prismaUIReceiveIme(d);})()"));
    }

    bool ImeHelper::HandleControlMessage(
        HWND window,
        UINT message,
        WPARAM wParam,
        [[maybe_unused]] LPARAM lParam,
        LRESULT& result) noexcept
    {
        if (message != AssociationMessage()) {
            return false;
        }
        if (window != window_) {
            result = FALSE;
            return true;
        }

        ApplyAssociationOnWindowThread(wParam != FALSE);
        result = TRUE;
        return true;
    }

    bool ImeHelper::HandleImeMessage(
        HWND window,
        UINT message,
        [[maybe_unused]] WPARAM wParam,
        LPARAM lParam) noexcept
    {
        if (window != window_ ||
            !associated_.load(std::memory_order_acquire)) {
            return false;
        }

        switch (message) {
        case WM_IME_STARTCOMPOSITION:
            PublishState(true);
            return true;

        case WM_IME_COMPOSITION: {
            const auto context = ImmGetContext(window);
            if (!context) {
                return true;
            }

            if ((lParam & GCS_RESULTSTR) != 0) {
                const auto committed = ReadComposition(
                    context,
                    GCS_RESULTSTR,
                    kMaximumCompositionCharacters);
                const auto callback = commitText_;
                const auto viewId = focusedView_.load(
                    std::memory_order_acquire);
                if (callback && viewId != 0 && !committed.empty()) {
                    callback(viewId, committed);
                }
            }
            ImmReleaseContext(window, context);
            PublishState((lParam & GCS_RESULTSTR) == 0);
            return true;
        }

        case WM_IME_ENDCOMPOSITION:
            PublishState(false);
            return true;

        case WM_IME_NOTIFY:
        case WM_IME_CONTROL:
        case WM_IME_REQUEST:
            PublishState(true);
            return true;

        case WM_IME_CHAR:
            // GCS_RESULTSTR is delivered above. Consuming WM_IME_CHAR avoids
            // submitting the same committed character twice.
            return true;

        default:
            return false;
        }
    }

    void ImeHelper::SuppressNativeUi(
        UINT message,
        LPARAM& lParam) noexcept
    {
        if (message == WM_IME_SETCONTEXT &&
            lParam != 0 &&
            associated_.load(std::memory_order_acquire)) {
            lParam &= ~static_cast<LPARAM>(
                ISC_SHOWUICOMPOSITIONWINDOW |
                ISC_SHOWUICANDIDATEWINDOW |
                ISC_SHOWUIGUIDELINE);
        }
    }

    void ImeHelper::ApplyAssociationOnWindowThread(
        bool enabled) noexcept
    {
        std::lock_guard lock(associationMutex_);
        if (!window_ || !ownedContext_) {
            associated_.store(false, std::memory_order_release);
            return;
        }

        const auto currentlyAssociated = associated_.load(
            std::memory_order_acquire);
        if (currentlyAssociated == enabled) {
            return;
        }

        if (enabled) {
            previousContext_ =
                ImmAssociateContext(window_, ownedContext_);
            associated_.store(true, std::memory_order_release);
        } else {
            ImmAssociateContext(window_, previousContext_);
            previousContext_ = nullptr;
            associated_.store(false, std::memory_order_release);
        }
    }

    void ImeHelper::PublishState(bool active) noexcept
    {
        const auto viewId = focusedView_.load(
            std::memory_order_acquire);
        if (viewId == 0) {
            return;
        }

        HWND window = nullptr;
        {
            std::lock_guard lock(associationMutex_);
            window = window_;
        }

        std::string script;
        if (active &&
            textInputFocused_.load(std::memory_order_acquire) &&
            window &&
            associated_.load(std::memory_order_acquire)) {
            const auto context = ImmGetContext(window);
            if (context) {
                script = BuildStateScript(context, true);
                ImmReleaseContext(window, context);
            }
        }
        if (script.empty()) {
            script =
                "(()=>{const d={active:false,composition:'',"
                "candidates:[],selection:0,caret:0};"
                "window.dispatchEvent(new CustomEvent("
                "'prismaui-ime-state',{detail:d}));"
                "if(typeof window.__prismaUIReceiveIme==='function')"
                "window.__prismaUIReceiveIme(d);})()";
        }

        Communication::Invoke(
            viewId,
            ultralight::String(script.c_str()));
    }

    std::wstring ImeHelper::ReadComposition(
        HIMC context,
        DWORD kind,
        std::size_t maximumCharacters) const
    {
        if (!context || maximumCharacters == 0) {
            return {};
        }

        const auto byteCount = ImmGetCompositionStringW(
            context,
            kind,
            nullptr,
            0);
        if (byteCount <= 0 ||
            static_cast<std::size_t>(byteCount) >
                maximumCharacters * sizeof(wchar_t) ||
            byteCount % sizeof(wchar_t) != 0) {
            return {};
        }

        std::wstring result(
            static_cast<std::size_t>(byteCount) /
                sizeof(wchar_t),
            L'\0');
        const auto read = ImmGetCompositionStringW(
            context,
            kind,
            result.data(),
            static_cast<DWORD>(byteCount));
        if (read != byteCount) {
            return {};
        }
        return result;
    }

    std::string ImeHelper::BuildStateScript(
        HIMC context,
        bool active) const
    {
        if (!context || !active) {
            return {};
        }

        const auto composition = ToUtf8(ReadComposition(
            context,
            GCS_COMPSTR,
            kMaximumCompositionCharacters));
        const auto cursor = ImmGetCompositionStringW(
            context,
            GCS_CURSORPOS,
            nullptr,
            0);

        std::ostringstream candidates;
        candidates << '[';
        DWORD selection = 0;
        const auto candidateBytes = ImmGetCandidateListW(
            context,
            0,
            nullptr,
            0);
        if (candidateBytes >= sizeof(CANDIDATELIST) &&
            candidateBytes <= kMaximumCandidateBytes) {
            std::vector<std::byte> storage(candidateBytes);
            const auto list = reinterpret_cast<LPCANDIDATELIST>(
                storage.data());
            if (ImmGetCandidateListW(
                    context,
                    0,
                    reinterpret_cast<LPCANDIDATELIST>(
                        storage.data()),
                    candidateBytes) == candidateBytes) {
                selection = list->dwSelection;
                const auto count = (std::min)(
                    static_cast<std::size_t>(list->dwCount),
                    kMaximumCandidates);
                const auto offsetTableBytes =
                    offsetof(CANDIDATELIST, dwOffset) +
                    count * sizeof(DWORD);
                if (offsetTableBytes <= candidateBytes) {
                    bool first = true;
                    for (std::size_t index = 0;
                         index < count;
                         ++index) {
                        const auto offset = list->dwOffset[index];
                        if (offset >= candidateBytes ||
                            offset % alignof(wchar_t) != 0) {
                            continue;
                        }

                        const auto availableBytes =
                            candidateBytes - offset;
                        const auto candidate = reinterpret_cast<
                            const wchar_t*>(
                            storage.data() + offset);
                        const auto availableCharacters =
                            availableBytes / sizeof(wchar_t);
                        std::size_t length = 0;
                        while (length < availableCharacters &&
                               candidate[length] != L'\0') {
                            ++length;
                        }
                        if (length == availableCharacters) {
                            continue;
                        }

                        if (!first) {
                            candidates << ',';
                        }
                        first = false;
                        candidates << '"'
                                   << EscapeJson(ToUtf8(
                                          std::wstring_view(
                                              candidate,
                                              length)))
                                   << '"';
                    }
                }
            }
        }
        candidates << ']';

        std::ostringstream script;
        script
            << "(()=>{const d={active:true,composition:\""
            << EscapeJson(composition)
            << "\",candidates:" << candidates.str()
            << ",selection:" << selection
            << ",caret:" << (cursor >= 0 ? cursor : 0)
            << "};window.dispatchEvent(new CustomEvent("
               "'prismaui-ime-state',{detail:d}));"
               "if(typeof window.__prismaUIReceiveIme==='function')"
               "window.__prismaUIReceiveIme(d);})()";
        return script.str();
    }
}
