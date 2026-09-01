#include "context_reader/runtime/runtime_executor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "context_reader/shared/error.hpp"

namespace context_reader {
namespace {

std::size_t default_worker_count() noexcept {
    const auto detected = static_cast<std::size_t>(std::thread::hardware_concurrency());
    return detected == 0 ? 2 : std::clamp(detected, std::size_t{1}, std::size_t{4});
}

}  // namespace

class RuntimeExecutor::Impl final {
public:
    Impl(std::size_t worker_count, RuntimeExecutorLimits limits)
        : worker_count_(worker_count), limits_(limits) {
        try {
            workers_.reserve(worker_count_);
            for(std::size_t index = 0; index < worker_count_; ++index) {
                workers_.emplace_back([this] { run(); });
            }
        } catch(...) {
            {
                const std::scoped_lock lock(mutex_);
                accepting_tasks_ = false;
            }
            wake_.notify_all();
            for(auto& worker : workers_) {
                if(worker.joinable()) worker.join();
            }
            throw;
        }
    }

    ~Impl() {
        {
            const std::scoped_lock lock(mutex_);
            accepting_tasks_ = false;
        }
        wake_.notify_all();
        for(auto& worker : workers_) {
            if(worker.joinable()) worker.join();
        }
    }

    Result<std::future<void>> submit(
        Task task,
        RuntimeTaskPriority priority,
        std::size_t estimated_bytes
    ) {
        if(!task || estimated_bytes == 0) {
            return Result<std::future<void>>::failure(
                Error(ErrorCode::invalid_argument, "runtime task and byte estimate must be valid")
            );
        }

        auto packaged_task = std::make_shared<std::packaged_task<void()>>(std::move(task));
        auto completion = packaged_task->get_future();

        try {
            const std::scoped_lock lock(mutex_);
            if(!accepting_tasks_) {
                return Result<std::future<void>>::failure(
                    Error(ErrorCode::cancelled, "runtime executor is shutting down")
                );
            }
            if(queue_.size() >= limits_.maximum_queue_items
               || estimated_bytes > limits_.maximum_queue_bytes -
                      std::min(queued_bytes_, limits_.maximum_queue_bytes)) {
                return Result<std::future<void>>::failure(
                    Error(ErrorCode::resource_exhausted, "runtime executor queue is full")
                );
            }
            queue_.push_back(QueuedTask{
                .priority = priority,
                .enqueued_at = std::chrono::steady_clock::now(),
                .estimated_bytes = estimated_bytes,
                .operation = [packaged_task] { (*packaged_task)(); },
            });
            queued_bytes_ += estimated_bytes;
        } catch(const std::exception& exception) {
            return Result<std::future<void>>::failure(
                Error(ErrorCode::resource_exhausted, exception.what())
            );
        }

        wake_.notify_one();
        return Result<std::future<void>>::success(std::move(completion));
    }

    [[nodiscard]] std::size_t concurrency() const noexcept { return worker_count_; }

private:
    struct QueuedTask final {
        RuntimeTaskPriority priority;
        std::chrono::steady_clock::time_point enqueued_at;
        std::size_t estimated_bytes;
        Task operation;
    };

    [[nodiscard]] std::size_t next_task_index(
        std::chrono::steady_clock::time_point now
    ) const {
        std::size_t selected = 0;
        auto selected_priority = static_cast<int>(RuntimeTaskPriority::indexing);
        for(std::size_t index = 0; index < queue_.size(); ++index) {
            const auto waited = now - queue_[index].enqueued_at;
            const auto promotions = limits_.priority_aging_interval.count() == 0
                ? static_cast<int>(RuntimeTaskPriority::indexing)
                : static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(waited).count() /
                    limits_.priority_aging_interval.count()
                );
            const auto effective = std::max(
                0,
                static_cast<int>(queue_[index].priority) - promotions
            );
            if(effective < selected_priority) {
                selected = index;
                selected_priority = effective;
            }
        }
        return selected;
    }

    void run() {
        for(;;) {
            QueuedTask task;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return !accepting_tasks_ || !queue_.empty(); });
                if(queue_.empty() && !accepting_tasks_) return;
                const auto index = next_task_index(std::chrono::steady_clock::now());
                task = std::move(queue_[index]);
                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index));
                queued_bytes_ -= task.estimated_bytes;
            }
            task.operation();
        }
    }

    std::size_t worker_count_;
    RuntimeExecutorLimits limits_;
    std::vector<std::thread> workers_;
    std::condition_variable wake_;
    std::deque<QueuedTask> queue_;
    std::size_t queued_bytes_{0};
    mutable std::mutex mutex_;
    bool accepting_tasks_{true};
};

Result<std::unique_ptr<RuntimeExecutor>> RuntimeExecutor::create(
    std::size_t worker_count,
    RuntimeExecutorLimits limits
) {
    if(worker_count == 0) {
        worker_count = default_worker_count();
    }
    if(worker_count == 0 || limits.maximum_queue_items == 0
       || limits.maximum_queue_bytes == 0 || limits.priority_aging_interval.count() < 0) {
        return Result<std::unique_ptr<RuntimeExecutor>>::failure(
            Error(ErrorCode::invalid_argument, "runtime executor limits are invalid")
        );
    }

    try {
        return Result<std::unique_ptr<RuntimeExecutor>>::success(
            std::unique_ptr<RuntimeExecutor>(
                new RuntimeExecutor(std::make_unique<Impl>(worker_count, limits))
            )
        );
    } catch(const std::exception& exception) {
        return Result<std::unique_ptr<RuntimeExecutor>>::failure(
            Error(ErrorCode::resource_exhausted, exception.what())
        );
    }
}

RuntimeExecutor::RuntimeExecutor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RuntimeExecutor::~RuntimeExecutor() = default;

Result<std::future<void>> RuntimeExecutor::submit(
    Task task,
    RuntimeTaskPriority priority,
    std::size_t estimated_bytes
) {
    return impl_->submit(std::move(task), priority, estimated_bytes);
}

std::size_t RuntimeExecutor::concurrency() const noexcept {
    return impl_->concurrency();
}

}  // namespace context_reader
