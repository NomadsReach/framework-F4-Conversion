#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class SingleThreadExecutor {
public:
    enum class Priority {
        HIGH = 0,
        MEDIUM = 1,
        LOW = 2,
    };

    SingleThreadExecutor();
    ~SingleThreadExecutor();

    SingleThreadExecutor(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor(SingleThreadExecutor&&) = delete;
    SingleThreadExecutor& operator=(SingleThreadExecutor&&) = delete;

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        return submit_with_priority(Priority::LOW, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto submit_with_priority(Priority priority, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();

        {
            std::lock_guard lock(queueMutex_);
            if (stop_) throw std::runtime_error("Executor is stopping");
            if (tasks_.size() >= kMaxQueuedTasks) throw std::runtime_error("Executor queue is full");
            tasks_.push_back(Task{priority, nextSequence_++, [task] { (*task)(); }});
            std::push_heap(tasks_.begin(), tasks_.end(), TaskCompare{});
        }

        condition_.notify_one();
        return future;
    }

    bool IsWorkerThread() const noexcept
    {
        return std::this_thread::get_id() == workerThreadId_;
    }

    void SetExceptionHandler(std::function<void(const std::exception_ptr&)> handler)
    {
        std::lock_guard lock(queueMutex_);
        exceptionHandler_ = std::move(handler);
    }

private:
    static constexpr std::size_t kMaxQueuedTasks = 4096;

    struct Task {
        Priority priority = Priority::LOW;
        uint64_t sequence = 0;
        std::function<void()> func;
    };

    struct TaskCompare {
        bool operator()(const Task& a, const Task& b) const noexcept
        {
            if (a.priority != b.priority) {
                return static_cast<int>(a.priority) > static_cast<int>(b.priority);
            }
            return a.sequence > b.sequence;
        }
    };

    void run();

    std::thread workerThread_;
    std::thread::id workerThreadId_{};
    std::vector<Task> tasks_;
    std::mutex queueMutex_;
    std::condition_variable condition_;
    bool stop_ = false;
    uint64_t nextSequence_ = 0;
    std::function<void(const std::exception_ptr&)> exceptionHandler_;
};

inline SingleThreadExecutor::SingleThreadExecutor()
{
    workerThread_ = std::thread(&SingleThreadExecutor::run, this);
    workerThreadId_ = workerThread_.get_id();
}

inline SingleThreadExecutor::~SingleThreadExecutor()
{
    {
        std::lock_guard lock(queueMutex_);
        stop_ = true;
        tasks_.clear();
    }
    condition_.notify_all();
    if (workerThread_.joinable()) workerThread_.join();
}

inline void SingleThreadExecutor::run()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(queueMutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_) return;

            std::pop_heap(tasks_.begin(), tasks_.end(), TaskCompare{});
            task = std::move(tasks_.back().func);
            tasks_.pop_back();
        }

        try {
            task();
        } catch (...) {
            std::function<void(const std::exception_ptr&)> handler;
            {
                std::lock_guard lock(queueMutex_);
                handler = exceptionHandler_;
            }
            if (handler) {
                try {
                    handler(std::current_exception());
                } catch (...) {
                }
            }
        }
    }
}
