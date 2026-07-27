#include "PCH.h"

#include "Utils/SingleThreadExecutor.h"

SingleThreadExecutor::~SingleThreadExecutor()
{
    (void)StopAndJoin(false);
}

bool SingleThreadExecutor::Start() noexcept
{
    std::lock_guard lock(mutex_);
    if (accepting_) {
        return true;
    }
    if (worker_.joinable()) {
        return false;
    }

    stopping_ = false;
    drainOnStop_ = false;
    accepting_ = true;
    try {
        worker_ = std::thread(&SingleThreadExecutor::Run, this);
    } catch (...) {
        accepting_ = false;
        return false;
    }
    return true;
}

bool SingleThreadExecutor::StopAndJoin(bool drainAcceptedTasks) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable()) {
            accepting_ = false;
            stopping_ = false;
            tasks_.clear();
            workerId_ = {};
            return true;
        }
        if (workerId_ == std::this_thread::get_id()) {
            return false;
        }

        accepting_ = false;
        stopping_ = true;
        drainOnStop_ = drainAcceptedTasks;
        if (!drainOnStop_) {
            tasks_.clear();
        }
    }
    condition_.notify_all();

    try {
        worker_.join();
    } catch (...) {
        return false;
    }

    std::lock_guard lock(mutex_);
    tasks_.clear();
    stopping_ = false;
    drainOnStop_ = false;
    workerId_ = {};
    return true;
}

bool SingleThreadExecutor::IsStarted() const noexcept
{
    std::lock_guard lock(mutex_);
    return accepting_ && worker_.joinable();
}

bool SingleThreadExecutor::IsWorkerThread() const noexcept
{
    std::lock_guard lock(mutex_);
    return workerId_ != std::thread::id{} &&
           workerId_ == std::this_thread::get_id();
}

std::size_t SingleThreadExecutor::QueuedTaskCount() const noexcept
{
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

bool SingleThreadExecutor::TryPost(
    Priority priority,
    std::function<void()> function) noexcept
{
    if (!function) {
        return false;
    }
    try {
        {
            std::lock_guard lock(mutex_);
            if (!accepting_ || tasks_.size() >= kMaximumQueuedTasks) {
                return false;
            }
            tasks_.push_back(Task{
                .priority = priority,
                .sequence = nextSequence_++,
                .function = std::move(function)
            });
            std::push_heap(tasks_.begin(), tasks_.end(), TaskCompare{});
        }
        condition_.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

void SingleThreadExecutor::Run() noexcept
{
    {
        std::lock_guard lock(mutex_);
        workerId_ = std::this_thread::get_id();
    }

    for (;;) {
        std::function<void()> function;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (stopping_ && (!drainOnStop_ || tasks_.empty())) {
                return;
            }
            if (tasks_.empty()) {
                continue;
            }

            std::pop_heap(tasks_.begin(), tasks_.end(), TaskCompare{});
            function = std::move(tasks_.back().function);
            tasks_.pop_back();
        }

        try {
            function();
        } catch (...) {
            logger::error("Unhandled exception escaped a PrismaUI worker task");
        }
    }
}
