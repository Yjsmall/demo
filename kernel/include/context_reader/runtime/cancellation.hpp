#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace context_reader {

class CancellationToken final {
public:
    CancellationToken() : state_(std::make_shared<std::atomic_bool>(false)) {}

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return state_->load(std::memory_order_acquire);
    }

private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<std::atomic_bool> state_;
};

class CancellationSource final {
public:
    CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}

    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken(state_);
    }

    void request_cancellation() const noexcept {
        state_->store(true, std::memory_order_release);
    }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

}  // namespace context_reader
