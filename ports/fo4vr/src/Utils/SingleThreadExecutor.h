#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class SingleThreadExecutor final
{
public:
    enum class Priority : std::uint8_t
    {
        FRAME_CRITICAL = 0,
        HIGH = 1,
        MEDIUM = 2,
        LOW = 3
    };

    static constexpr std::size_t kMaximumQueuedTasks = 4096;

    SingleThreadExecutor() = default;
    ~SingleThreadExecutor();

    SingleThreadExecutor(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor(SingleThreadExecutor&&) = delete;
    SingleThreadExecutor& operator=(SingleThreadExecutor&&) = delete;

    [[nodiscard]] bool Start() noexcept;
    [[nodiscard]] bool StopAndJoin(bool drainAcceptedTasks = false) noexcept;
    [[nodiscard]] bool IsStarted() const noexcept;
    [[nodiscard]] bool IsWorkerThread() const noexcept;
    [[nodiscard]] std::size_t QueuedTaskCount() const noexcept;
    [[nodiscard]] bool TryPost(
        Priority priority,
        std::function<void()> function) noexcept;

    template <class Function, class... Arguments>
    auto submit(Function&& function, Arguments&&... arguments)
        -> std::future<std::invoke_result_t<Function, Arguments...>>
    {
        return submit_with_priority(
            Priority::LOW,
            std::forward<Function>(function),
            std::forward<Arguments>(arguments)...);
    }

    template <class Function, class... Arguments>
    auto submit_with_priority(
        Priority priority,
        Function&& function,
        Arguments&&... arguments)
        -> std::future<std::invoke_result_t<Function, Arguments...>>
    {
        using Result = std::invoke_result_t<Function, Arguments...>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::bind(
                std::forward<Function>(function),
                std::forward<Arguments>(arguments)...));
        auto future = task->get_future();

        {
            std::lock_guard lock(mutex_);
            if (!accepting_ || tasks_.size() >= kMaximumQueuedTasks) {
                std::promise<Result> rejected;
                auto rejectedFuture = rejected.get_future();
                rejected.set_exception(std::make_exception_ptr(std::runtime_error(
                    accepting_ ?
                        "PrismaUI executor queue limit reached" :
                        "PrismaUI executor is stopped")));
                return rejectedFuture;
            }

            tasks_.push_back(Task{
                .priority = priority,
                .sequence = nextSequence_++,
                .function = [task] { (*task)(); }
            });
            std::push_heap(tasks_.begin(), tasks_.end(), TaskCompare{});
        }

        condition_.notify_one();
        return future;
    }

private:
    struct Task
    {
        Priority priority = Priority::LOW;
        std::uint64_t sequence = 0;
        std::function<void()> function;
    };

    struct TaskCompare
    {
        [[nodiscard]] bool operator()(const Task& left, const Task& right) const noexcept
        {
            if (left.priority != right.priority) {
                return static_cast<unsigned>(left.priority) >
                       static_cast<unsigned>(right.priority);
            }
            return left.sequence > right.sequence;
        }
    };

    void Run() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    std::thread::id workerId_{};
    std::vector<Task> tasks_;
    std::uint64_t nextSequence_ = 0;
    bool accepting_ = false;
    bool stopping_ = false;
    bool drainOnStop_ = false;
};
