#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>

#include "context_reader/shared/result.hpp"

namespace context_reader {

enum class RuntimeTaskPriority : std::uint8_t {
    visible_tile = 0,
    adjacent_page = 1,
    thumbnail = 2,
    indexing = 3,
};

struct RuntimeExecutorLimits final {
    std::size_t maximum_queue_items{256};
    std::size_t maximum_queue_bytes{32U * 1024U * 1024U};
    std::chrono::milliseconds priority_aging_interval{std::chrono::seconds(2)};
};

class RuntimeExecutor final {
public:
    using Task = std::function<void()>;

    [[nodiscard]] static Result<std::unique_ptr<RuntimeExecutor>> create(
        std::size_t worker_count = 0,
        RuntimeExecutorLimits limits = RuntimeExecutorLimits{}
    );

    RuntimeExecutor(const RuntimeExecutor&) = delete;
    RuntimeExecutor& operator=(const RuntimeExecutor&) = delete;
    RuntimeExecutor(RuntimeExecutor&&) = delete;
    RuntimeExecutor& operator=(RuntimeExecutor&&) = delete;
    ~RuntimeExecutor();

    [[nodiscard]] Result<std::future<void>> submit(
        Task task,
        RuntimeTaskPriority priority = RuntimeTaskPriority::adjacent_page,
        std::size_t estimated_bytes = 1
    );
    [[nodiscard]] std::size_t concurrency() const noexcept;

private:
    class Impl;

    explicit RuntimeExecutor(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace context_reader
