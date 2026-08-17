#pragma once

#include "Core.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>

class SingleThreadExecutor;

namespace PrismaUI {

using ImeEscapeForJSCallback = std::function<std::string(const std::string&)>;
using ImeQueueCommittedCharCallback = std::function<void(const std::wstring&, LPARAM)>;
using ImeConvertUtf16ToUtf8Callback = std::function<std::string(const wchar_t*, int)>;

struct ImeHelperContext {
    HWND hwnd = nullptr;
    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap = nullptr;
    std::shared_mutex* viewsMapMutex = nullptr;
    std::mutex* focusedViewIdMutex = nullptr;
    Core::PrismaViewId* currentlyFocusedViewId = nullptr;
    std::atomic<bool>* isAnyInputCaptureActive = nullptr;
    std::atomic<bool>* isTextInputFocused = nullptr;
};

class ImeHelper {
public:
    ImeHelper() = default;
    ~ImeHelper() = default;

    void Initialize(HWND hwnd);
    void Shutdown(HWND hwnd);
    void SetCallbacks(ImeEscapeForJSCallback escapeForJS, ImeQueueCommittedCharCallback queueCommittedChar,
                      ImeConvertUtf16ToUtf8Callback convertUtf16ToUtf8);
    void SetContext(const ImeHelperContext& ctx);
    void SetExecutor(SingleThreadExecutor* executor);

    // ImmAssociateContext must run on the window thread.
    void SetAssociation(bool enabled);

    // Runs on the Ultralight worker or through its executor.
    void UpdateStateForFocusedView(Core::PrismaViewId viewId);

    void SendStateToJS(Core::PrismaViewId viewId, HWND hwnd, bool active);
    void ClearStateInJS(Core::PrismaViewId viewId);
    bool IsAssociated() const { return m_associated.load(); }
    bool HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, Core::PrismaViewId focusedViewId,
                       bool* outHandled);
    bool HandleControlMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* outResult);

    // Apply before DefSubclassProc handles WM_IME_SETCONTEXT.
    void ModifySetContextLParam(LPARAM* lParam, UINT uMsg);

    static const char* MessageName(UINT uMsg);

private:
    void DispatchScriptToView(Core::PrismaViewId viewId, const std::string& script);
    bool IsTextInputFocused() const;
    void UpdateStateImpl(Core::PrismaViewId viewId);

    HIMC m_context = nullptr;
    bool m_contextOwned = false;
    std::atomic<bool> m_associated{false};
    std::atomic<bool> m_lastKnownTextInputFocus{false};

    ImeEscapeForJSCallback m_escapeForJS;
    ImeQueueCommittedCharCallback m_queueCommittedChar;
    ImeConvertUtf16ToUtf8Callback m_convertUtf16ToUtf8;
    ImeHelperContext m_ctx;
    SingleThreadExecutor* m_executor = nullptr;
};

}
