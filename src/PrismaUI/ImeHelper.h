#pragma once

#include "Core.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
    void Initialize(HWND hwnd);
    void Shutdown(HWND hwnd);
    void SetCallbacks(ImeEscapeForJSCallback escapeForJS, ImeQueueCommittedCharCallback queueCommittedChar,
                      ImeConvertUtf16ToUtf8Callback convertUtf16ToUtf8);
    void SetContext(const ImeHelperContext& ctx);
    void SetExecutor(SingleThreadExecutor* executor);
    void SetAssociation(bool enabled);
    void UpdateStateForFocusedView(Core::PrismaViewId viewId);
    void SendStateToJS(Core::PrismaViewId viewId, HWND hwnd, bool active);
    void ClearStateInJS(Core::PrismaViewId viewId);
    bool IsAssociated() const { return m_associated.load(std::memory_order_acquire); }
    bool HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, Core::PrismaViewId focusedViewId,
                       bool* outHandled);
    bool HandleControlMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* outResult);
    void ModifySetContextLParam(LPARAM* lParam, UINT uMsg);
    static const char* MessageName(UINT uMsg);

private:
    bool EnsureContext(HWND hwnd);
    void DispatchScriptToView(Core::PrismaViewId viewId, const std::string& script);
    bool IsTextInputFocused() const;
    void UpdateStateImpl(Core::PrismaViewId viewId);

    std::mutex m_contextMutex;
    HWND m_window = nullptr;
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
