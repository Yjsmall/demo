#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <memory>

#include "context_reader/shared/result.hpp"

namespace context_reader {

class RuntimeExecutor final {
public:
    using Task = std::function<void()>;

    [[nodiscard]] static Result<std::unique_ptr<RuntimeExecutor>> create(
        std::size_t worker_count = 0
    );

    RuntimeExecutor(const RuntimeExecutor&) = delete;
    RuntimeExecutor& operator=(const RuntimeExecutor&) = delete;
    RuntimeExecutor(RuntimeExecutor&&) = delete;
    RuntimeExecutor& operator=(RuntimeExecutor&&) = delete;
    ~RuntimeExecutor();

    [[nodiscard]] Result<std::future<void>> submit(Task task);
    [[nodiscard]] std::size_t concurrency() const noexcept;

private:
    class Impl;

    explicit RuntimeExecutor(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace context_reader
