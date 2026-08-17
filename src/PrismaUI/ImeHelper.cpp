#include "ImeHelper.h"

#include <cstddef>
#include <cstdio>
#include <imm.h>
#include <vector>

#pragma comment(lib, "imm32.lib")

#include "Communication.h"
#include "Core.h"

namespace PrismaUI {
namespace {

struct ImeCandidateState {
    std::vector<std::string> candidates;
    int selectedIndex = -1;
};

struct ImeUiState {
    bool active = false;
    std::string composition;
    int caret = 0;
    ImeCandidateState candidates;
};

UINT AssociationMessageId()
{
    static const UINT id = RegisterWindowMessageW(L"PrismaUI.ImeAssociation.v2");
    return id;
}

std::string EscapeJson(const std::string& value)
{
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            default:
                if (c < 0x20) {
                    char hex[7]{};
                    std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned>(c));
                    output += hex;
                } else {
                    output.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return output;
}

bool ReadCandidate(const std::vector<std::byte>& bytes, DWORD offset, std::wstring& output)
{
    if (offset >= bytes.size() || (offset % alignof(wchar_t)) != 0) return false;
    const auto* text = reinterpret_cast<const wchar_t*>(bytes.data() + offset);
    const size_t maxChars = (bytes.size() - offset) / sizeof(wchar_t);
    size_t length = 0;
    while (length < maxChars && text[length] != L'\0') ++length;
    if (length == maxChars) return false;
    output.assign(text, length);
    return true;
}

}

const char* ImeHelper::MessageName(UINT message)
{
    switch (message) {
        case WM_GETDLGCODE: return "WM_GETDLGCODE";
        case WM_INPUTLANGCHANGE: return "WM_INPUTLANGCHANGE";
        case WM_IME_SETCONTEXT: return "WM_IME_SETCONTEXT";
        case WM_IME_STARTCOMPOSITION: return "WM_IME_STARTCOMPOSITION";
        case WM_IME_COMPOSITION: return "WM_IME_COMPOSITION";
        case WM_IME_ENDCOMPOSITION: return "WM_IME_ENDCOMPOSITION";
        case WM_IME_CHAR: return "WM_IME_CHAR";
        case WM_IME_NOTIFY: return "WM_IME_NOTIFY";
        default: return "WM_IME_?";
    }
}

void ImeHelper::SetCallbacks(ImeEscapeForJSCallback escapeForJS,
                             ImeQueueCommittedCharCallback queueCommittedChar,
                             ImeConvertUtf16ToUtf8Callback convertUtf16ToUtf8)
{
    m_escapeForJS = std::move(escapeForJS);
    m_queueCommittedChar = std::move(queueCommittedChar);
    m_convertUtf16ToUtf8 = std::move(convertUtf16ToUtf8);
}

void ImeHelper::SetContext(const ImeHelperContext& ctx)
{
    m_ctx = ctx;
}

void ImeHelper::SetExecutor(SingleThreadExecutor* executor)
{
    m_executor = executor;
}

void ImeHelper::Initialize(HWND hwnd)
{
    std::lock_guard lock(m_contextMutex);
    m_window = hwnd;
    m_associated.store(false, std::memory_order_release);
}

bool ImeHelper::EnsureContext(HWND hwnd)
{
    if (!hwnd) return false;

    DWORD processId = 0;
    const DWORD ownerThread = GetWindowThreadProcessId(hwnd, &processId);
    if (!ownerThread || processId != GetCurrentProcessId() || ownerThread != GetCurrentThreadId()) return false;

    std::lock_guard lock(m_contextMutex);
    if (m_context) return true;

    HIMC previous = ImmAssociateContext(hwnd, nullptr);
    if (previous) {
        m_context = previous;
        m_contextOwned = false;
    } else {
        m_context = ImmCreateContext();
        m_contextOwned = m_context != nullptr;
    }

    m_associated.store(false, std::memory_order_release);
    return m_context != nullptr;
}

void ImeHelper::Shutdown(HWND hwnd)
{
    if (!hwnd) return;

    DWORD processId = 0;
    const DWORD ownerThread = GetWindowThreadProcessId(hwnd, &processId);
    if (!ownerThread || processId != GetCurrentProcessId() || ownerThread != GetCurrentThreadId()) {
        SetAssociation(false);
        return;
    }

    std::lock_guard lock(m_contextMutex);
    m_associated.store(false, std::memory_order_release);
    if (m_context) {
        if (m_contextOwned) {
            ImmAssociateContext(hwnd, nullptr);
            ImmDestroyContext(m_context);
        } else {
            ImmAssociateContext(hwnd, m_context);
        }
    }
    m_context = nullptr;
    m_contextOwned = false;
    m_window = nullptr;
}

void ImeHelper::SetAssociation(bool enabled)
{
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(m_contextMutex);
        hwnd = m_window;
    }
    if (!hwnd) return;

    if (!PostMessageW(hwnd, AssociationMessageId(), enabled ? 1 : 0, 0)) {
        logger::warn("IME association message failed, GLE={}", GetLastError());
    }
}

bool ImeHelper::HandleControlMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM, LRESULT* outResult)
{
    if (message != AssociationMessageId()) return false;

    const bool enabled = wParam != 0;
    if (!EnsureContext(hwnd)) {
        m_associated.store(false, std::memory_order_release);
        if (outResult) *outResult = 0;
        return true;
    }

    HIMC context = nullptr;
    {
        std::lock_guard lock(m_contextMutex);
        context = m_context;
    }

    ImmAssociateContext(hwnd, enabled ? context : nullptr);
    m_associated.store(enabled && context != nullptr, std::memory_order_release);
    if (outResult) *outResult = 0;
    return true;
}

void ImeHelper::ModifySetContextLParam(LPARAM* lParam, UINT message)
{
    if (message == WM_IME_SETCONTEXT && lParam) *lParam = 0;
}

bool ImeHelper::IsTextInputFocused() const
{
    return m_ctx.isTextInputFocused && m_ctx.isTextInputFocused->load(std::memory_order_acquire);
}

void ImeHelper::DispatchScriptToView(Core::PrismaViewId viewId, const std::string& script)
{
    if (!viewId || script.empty()) return;

    if (m_executor && m_executor->IsWorkerThread() && m_ctx.viewsMap && m_ctx.viewsMapMutex) {
        std::shared_ptr<Core::PrismaView> viewData;
        {
            std::shared_lock lock(*m_ctx.viewsMapMutex);
            auto it = m_ctx.viewsMap->find(viewId);
            if (it != m_ctx.viewsMap->end()) viewData = it->second;
        }
        if (!viewData || viewData->isDestroying.load(std::memory_order_acquire) || !viewData->ultralightView) return;

        try {
            viewData->ultralightView->EvaluateScript(ultralight::String(script.c_str()), nullptr);
        } catch (const std::exception& e) {
            logger::error("IME script dispatch failed for View [{}]: {}", viewId, e.what());
        } catch (...) {
            logger::error("IME script dispatch failed for View [{}]", viewId);
        }
        return;
    }

    Communication::Invoke(viewId, ultralight::String(script.c_str()), nullptr);
}

void ImeHelper::ClearStateInJS(Core::PrismaViewId viewId)
{
    SendStateToJS(viewId, nullptr, false);
}

void ImeHelper::SendStateToJS(Core::PrismaViewId viewId, HWND hwnd, bool active)
{
    if (!viewId || !m_escapeForJS || !m_convertUtf16ToUtf8) return;

    ImeUiState state;
    state.active = active;

    if (hwnd) {
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
            const LONG compositionBytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
            if (compositionBytes > 0 && (compositionBytes % sizeof(wchar_t)) == 0) {
                std::vector<wchar_t> buffer(static_cast<size_t>(compositionBytes) / sizeof(wchar_t));
                const LONG copied = ImmGetCompositionStringW(himc, GCS_COMPSTR, buffer.data(), compositionBytes);
                if (copied > 0 && (copied % sizeof(wchar_t)) == 0) {
                    state.composition = m_convertUtf16ToUtf8(buffer.data(), copied / sizeof(wchar_t));
                }
            }

            const LONG cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
            if (cursor >= 0) state.caret = static_cast<int>(cursor);

            const DWORD candidateBytes = ImmGetCandidateListW(himc, 0, nullptr, 0);
            if (candidateBytes >= sizeof(CANDIDATELIST)) {
                std::vector<std::byte> buffer(candidateBytes);
                const DWORD copied = ImmGetCandidateListW(
                    himc, 0, reinterpret_cast<LPCANDIDATELIST>(buffer.data()), candidateBytes);
                if (copied >= sizeof(CANDIDATELIST) && copied <= buffer.size()) {
                    buffer.resize(copied);
                    auto* list = reinterpret_cast<LPCANDIDATELIST>(buffer.data());
                    const size_t offsetTableBytes = offsetof(CANDIDATELIST, dwOffset) +
                                                    static_cast<size_t>(list->dwCount) * sizeof(DWORD);
                    if (offsetTableBytes <= buffer.size()) {
                        const DWORD pageStart = list->dwPageStart < list->dwCount ? list->dwPageStart : 0;
                        const DWORD pageSize = list->dwPageSize == 0 ? list->dwCount : list->dwPageSize;
                        const DWORD pageEnd = std::min<DWORD>(list->dwCount, pageStart + pageSize);
                        for (DWORD index = pageStart; index < pageEnd; ++index) {
                            std::wstring candidate;
                            if (!ReadCandidate(buffer, list->dwOffset[index], candidate)) continue;
                            state.candidates.candidates.push_back(
                                m_convertUtf16ToUtf8(candidate.data(), static_cast<int>(candidate.size())));
                        }
                        if (list->dwSelection >= pageStart && list->dwSelection < pageEnd) {
                            state.candidates.selectedIndex = static_cast<int>(list->dwSelection - pageStart);
                        }
                    }
                }
            }
            ImmReleaseContext(hwnd, himc);
        }
    }

    std::string json = "{\"active\":";
    json += state.active ? "true" : "false";
    json += ",\"composition\":\"" + EscapeJson(state.composition) + "\"";
    json += ",\"caret\":" + std::to_string(state.caret);
    json += ",\"selectedIndex\":" + std::to_string(state.candidates.selectedIndex);
    json += ",\"candidates\":[";
    for (size_t i = 0; i < state.candidates.candidates.size(); ++i) {
        if (i) json.push_back(',');
        json += "\"" + EscapeJson(state.candidates.candidates[i]) + "\"";
    }
    json += "]}";

    const std::string escaped = m_escapeForJS(json);
    if (escaped.empty()) return;
    DispatchScriptToView(viewId,
        "window.dispatchEvent(new CustomEvent('prismaIME_state',{detail:JSON.parse('" + escaped + "')}))");
}

bool ImeHelper::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                              Core::PrismaViewId focusedViewId, bool* outHandled)
{
    if (!outHandled || focusedViewId == 0 || !m_associated.load(std::memory_order_acquire) || !IsTextInputFocused()) {
        return false;
    }

    switch (message) {
        case WM_IME_STARTCOMPOSITION:
            *outHandled = true;
            SendStateToJS(focusedViewId, hwnd, true);
            return true;
        case WM_IME_ENDCOMPOSITION:
            *outHandled = true;
            ClearStateInJS(focusedViewId);
            return true;
        case WM_IME_CHAR:
            *outHandled = true;
            return true;
        case WM_IME_COMPOSITION: {
            *outHandled = true;
            if (m_queueCommittedChar && (lParam & GCS_RESULTSTR)) {
                HIMC himc = ImmGetContext(hwnd);
                if (himc) {
                    const LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0);
                    if (bytes > 0 && (bytes % sizeof(wchar_t)) == 0) {
                        std::vector<wchar_t> buffer(static_cast<size_t>(bytes) / sizeof(wchar_t));
                        const LONG copied = ImmGetCompositionStringW(himc, GCS_RESULTSTR, buffer.data(), bytes);
                        if (copied > 0 && (copied % sizeof(wchar_t)) == 0) {
                            m_queueCommittedChar(std::wstring(buffer.data(), copied / sizeof(wchar_t)), lParam);
                        }
                    }
                    ImmReleaseContext(hwnd, himc);
                }
            }
            SendStateToJS(focusedViewId, hwnd, true);
            return true;
        }
        case WM_IME_NOTIFY:
            if (wParam == IMN_CHANGECANDIDATE || wParam == IMN_OPENCANDIDATE || wParam == IMN_CLOSECANDIDATE) {
                *outHandled = true;
                SendStateToJS(focusedViewId, hwnd, true);
            }
            return *outHandled;
        default:
            return false;
    }
}

void ImeHelper::UpdateStateImpl(Core::PrismaViewId viewId)
{
    if (!m_ctx.viewsMap || !m_ctx.viewsMapMutex || viewId == 0) {
        m_lastKnownTextInputFocus.store(false, std::memory_order_release);
        return;
    }

    bool focused = false;
    if (m_ctx.focusedViewIdMutex && m_ctx.currentlyFocusedViewId && m_ctx.isAnyInputCaptureActive) {
        std::lock_guard lock(*m_ctx.focusedViewIdMutex);
        focused = m_ctx.isAnyInputCaptureActive->load(std::memory_order_acquire) &&
                  *m_ctx.currentlyFocusedViewId == viewId;
    }

    if (!focused) {
        m_lastKnownTextInputFocus.store(false, std::memory_order_release);
        ClearStateInJS(viewId);
        return;
    }

    const bool textFocused = IsTextInputFocused();
    m_lastKnownTextInputFocus.store(textFocused, std::memory_order_release);
    if (!textFocused) ClearStateInJS(viewId);
}

void ImeHelper::UpdateStateForFocusedView(Core::PrismaViewId viewId)
{
    if (!m_executor) return;
    if (m_executor->IsWorkerThread()) {
        UpdateStateImpl(viewId);
        return;
    }
    m_executor->submit_with_priority(SingleThreadExecutor::Priority::HIGH,
        [this, viewId] { UpdateStateImpl(viewId); });
}

}
