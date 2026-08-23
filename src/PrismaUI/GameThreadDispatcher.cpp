#include "GameThreadDispatcher.h"

#include "GameThreadDispatchQueue.h"
#include "ViewManager.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <utility>

namespace PrismaUI::GameThreadDispatcher {
namespace {

constexpr auto kWakeBudget = std::chrono::milliseconds(2);
constexpr auto kDestroyWait = std::chrono::milliseconds(250);

std::atomic<DWORD> g_gameThreadId{0};
std::atomic<DWORD> g_drainThreadId{0};
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};
GameThreadDispatchQueue::Queue g_tasks;

void FailClosed(const char* reason, DWORD expected, DWORD actual) noexcept
{
    g_ready.store(false, std::memory_order_release);
    g_failed.store(true, std::memory_order_release);
    g_window.store(nullptr, std::memory_order_release);
    g_tasks.failClosed();
    logger::critical("[GameThreadDispatcher] {} (expected thread={}, actual thread={})", reason, expected, actual);
}

bool PostWake() noexcept
{
    const HWND hwnd = g_window.load(std::memory_order_acquire);
    const UINT message = MessageId();
    if (!hwnd || !message) return false;
    if (::PostMessageW(hwnd, message, 0, 0)) return true;
    FailClosed("PostMessageW failed", g_gameThreadId.load(std::memory_order_acquire), ::GetCurrentThreadId());
    return false;
}

}

UINT MessageId() noexcept
{
    static const UINT id = ::RegisterWindowMessageW(L"PrismaUI_F4.GameThreadDispatch.v1");
    return id;
}

void CaptureCurrentThread() noexcept
{
    const DWORD current = ::GetCurrentThreadId();
    DWORD expected = 0;
    if (g_gameThreadId.compare_exchange_strong(expected, current, std::memory_order_acq_rel)) {
        logger::info("[GameThreadDispatcher] captured plugin-load thread {}", current);
        return;
    }
    if (expected != current) FailClosed("plugin-load thread changed", expected, current);
}

bool AttachWindow(HWND hwnd) noexcept
{
    if (!hwnd || g_failed.load(std::memory_order_acquire)) return false;

    const DWORD expectedThread = g_gameThreadId.load(std::memory_order_acquire);
    if (!MessageId()) {
        FailClosed("RegisterWindowMessageW failed", expectedThread, 0);
        return false;
    }

    DWORD processId = 0;
    const DWORD ownerThread = ::GetWindowThreadProcessId(hwnd, &processId);
    const DWORD currentThread = ::GetCurrentThreadId();
    if (!expectedThread || !ownerThread || processId != ::GetCurrentProcessId()) {
        FailClosed("Fallout window ownership could not be verified", expectedThread, ownerThread);
        return false;
    }
    if (ownerThread != expectedThread || currentThread != ownerThread) {
        FailClosed("Fallout window thread mismatch", expectedThread, currentThread);
        return false;
    }

    const HWND existing = g_window.load(std::memory_order_acquire);
    if (existing == hwnd && g_ready.load(std::memory_order_acquire)) return true;

    g_window.store(hwnd, std::memory_order_release);
    g_tasks.attach();
    g_ready.store(true, std::memory_order_release);
    logger::info("[GameThreadDispatcher] attached HWND {:p} on thread {}", static_cast<void*>(hwnd), ownerThread);
    return true;
}

void DetachWindow(HWND hwnd) noexcept
{
    HWND expected = hwnd;
    if (g_window.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
        g_ready.store(false, std::memory_order_release);
        g_tasks.detach();
    }
}

bool IsReady() noexcept
{
    return g_ready.load(std::memory_order_acquire) && !g_failed.load(std::memory_order_acquire);
}

bool IsGameThread() noexcept
{
    return IsReady() && ::GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_acquire);
}

bool Dispatch(std::function<void()> task, uint64_t view)
{
    if (!task || g_failed.load(std::memory_order_acquire)) return false;
    const auto queued = g_tasks.tryEnqueue(std::move(task), view);
    if (!queued.accepted) return false;
    if (!queued.needsWake) return true;
    return PostWake();
}

bool DispatchSafety(std::function<void()> task)
{
    if (!task || g_failed.load(std::memory_order_acquire)) return false;
    const auto queued = g_tasks.tryEnqueue(std::move(task), 0, true);
    if (!queued.accepted) return false;
    if (!queued.needsWake) return true;
    return PostWake();
}

void DropView(uint64_t view) noexcept
{
    g_tasks.dropView(view);
    const DWORD drainThread = g_drainThreadId.load(std::memory_order_acquire);
    if (drainThread != 0 && ::GetCurrentThreadId() == drainThread) return;
    if (!g_tasks.waitUntilNotExecutingFor(view, kDestroyWait)) {
        logger::warn("[GameThreadDispatcher] View {} destroyed while its callback was still running", view);
    }
}

bool HandleWindowMessage(HWND hwnd, UINT message) noexcept
{
    const UINT registeredMessage = MessageId();
    if (!registeredMessage || message != registeredMessage) return false;

    const DWORD expectedThread = g_gameThreadId.load(std::memory_order_acquire);
    const DWORD currentThread = ::GetCurrentThreadId();
    const HWND liveWindow = g_window.load(std::memory_order_acquire);
    if (!IsReady() || hwnd != liveWindow) return true;
    if (currentThread != expectedThread) {
        FailClosed("dispatch message arrived on the wrong thread", expectedThread, currentThread);
        return true;
    }

    thread_local bool insideDrain = false;
    if (insideDrain) return true;

    struct DrainFlag {
        explicit DrainFlag(bool& flag) : flag_(flag) { flag_ = true; }
        ~DrainFlag() { flag_ = false; }
        bool& flag_;
    } drainFlag{insideDrain};

    struct DrainThread {
        explicit DrainThread(DWORD id) { g_drainThreadId.store(id, std::memory_order_release); }
        ~DrainThread() { g_drainThreadId.store(0, std::memory_order_release); }
    } drainThread{currentThread};

    g_tasks.beginDrain();
    const auto deadline = std::chrono::steady_clock::now() + kWakeBudget;
    std::size_t ran = 0;
    while (ran < GameThreadDispatchQueue::kMaxTasksPerWake) {
        if (ran > 0 && std::chrono::steady_clock::now() >= deadline) break;
        auto task = g_tasks.takeNext([](uint64_t view) { return view == 0 || ViewManager::IsValid(view); });
        if (!task) break;
        ++ran;
        try {
            if (task->fn) task->fn();
        } catch (const std::exception& e) {
            logger::error("[GameThreadDispatcher] callback threw: {}", e.what());
        } catch (...) {
            logger::error("[GameThreadDispatcher] callback threw");
        }
        g_tasks.finishExecuting();
    }
    g_tasks.endDrain();

    if (g_tasks.takeWakeRearm()) (void)PostWake();
    return true;
}

}
