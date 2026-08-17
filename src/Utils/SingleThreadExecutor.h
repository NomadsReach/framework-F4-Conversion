#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
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
        LOW = 2
    };

    SingleThreadExecutor();
    ~SingleThreadExecutor();

    SingleThreadExecutor(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor(SingleThreadExecutor&&) = delete;
    SingleThreadExecutor& operator=(SingleThreadExecutor&&) = delete;

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        return submit_with_priority(Priority::LOW, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto submit_with_priority(Priority priority, F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto taskPtr = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<ReturnType> result = taskPtr->get_future();

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_) {
                throw std::runtime_error("Executor is stopping");
            }
            tasks_.push_back({priority, [taskPtr]() { (*taskPtr)(); }});
            std::push_heap(tasks_.begin(), tasks_.end(), TaskCompare());
        }

        condition_.notify_one();
        return result;
    }

    bool IsWorkerThread() const {
        return std::this_thread::get_id() == workerThreadId_.load();
    }

    void SetExceptionHandler(std::function<void(const std::exception_ptr&)> handler) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        exceptionHandler_ = std::move(handler);
    }

private:
    struct Task {
        Priority priority;
        std::function<void()> func;
    };

    struct TaskCompare {
        bool operator()(const Task& a, const Task& b) const {
            return static_cast<int>(a.priority) > static_cast<int>(b.priority);
        }
    };

    void run();

    std::thread workerThread_;
    std::atomic<std::thread::id> workerThreadId_;
    std::vector<Task> tasks_;
    std::mutex queueMutex_;
    std::condition_variable condition_;
    bool stop_;
    std::function<void(const std::exception_ptr&)> exceptionHandler_;
};

inline SingleThreadExecutor::SingleThreadExecutor() : stop_(false), workerThreadId_(), exceptionHandler_(nullptr) {
    workerThread_ = std::thread(&SingleThreadExecutor::run, this);
}

inline SingleThreadExecutor::~SingleThreadExecutor() {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        stop_ = true;
    }
    condition_.notify_one();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

inline void SingleThreadExecutor::run() {
    workerThreadId_.store(std::this_thread::get_id());

    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) {
                return;
            }

            std::pop_heap(tasks_.begin(), tasks_.end(), TaskCompare());
            task = std::move(tasks_.back().func);
            tasks_.pop_back();
        }

        try {
            task();
        } catch (...) {
            std::function<void(const std::exception_ptr&)> handler;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
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
