#include "context_reader/runtime/runtime_executor.hpp"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

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
    explicit Impl(std::size_t worker_count)
        : worker_count_(worker_count), pool_(worker_count) {}

    ~Impl() {
        {
            const std::scoped_lock lock(mutex_);
            accepting_tasks_ = false;
        }
        pool_.join();
    }

    Result<std::future<void>> submit(Task task) {
        if(!task) {
            return Result<std::future<void>>::failure(
                Error(ErrorCode::invalid_argument, "runtime task must not be empty")
            );
        }

        std::packaged_task<void()> packaged_task(std::move(task));
        auto completion = packaged_task.get_future();

        try {
            const std::scoped_lock lock(mutex_);
            if(!accepting_tasks_) {
                return Result<std::future<void>>::failure(
                    Error(ErrorCode::cancelled, "runtime executor is shutting down")
                );
            }
            asio::post(
                pool_,
                [task_to_run = std::move(packaged_task)]() mutable { task_to_run(); }
            );
        } catch(const std::exception& exception) {
            return Result<std::future<void>>::failure(
                Error(ErrorCode::resource_exhausted, exception.what())
            );
        }

        return Result<std::future<void>>::success(std::move(completion));
    }

    [[nodiscard]] std::size_t concurrency() const noexcept { return worker_count_; }

private:
    std::size_t worker_count_;
    asio::thread_pool pool_;
    mutable std::mutex mutex_;
    bool accepting_tasks_{true};
};

Result<std::unique_ptr<RuntimeExecutor>> RuntimeExecutor::create(std::size_t worker_count) {
    if(worker_count == 0) {
        worker_count = default_worker_count();
    }

    try {
        return Result<std::unique_ptr<RuntimeExecutor>>::success(
            std::unique_ptr<RuntimeExecutor>(
                new RuntimeExecutor(std::make_unique<Impl>(worker_count))
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

Result<std::future<void>> RuntimeExecutor::submit(Task task) {
    return impl_->submit(std::move(task));
}

std::size_t RuntimeExecutor::concurrency() const noexcept {
    return impl_->concurrency();
}

}  // namespace context_reader
