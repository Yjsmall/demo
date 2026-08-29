#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#include "context_reader/shared/error.hpp"

namespace context_reader {

template <typename T>
class [[nodiscard]] Result final {
    static_assert(!std::is_reference_v<T>);
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>);

public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(state_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<T>(state_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(state_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(state_)); }

    [[nodiscard]] Error& error() & { return std::get<Error>(state_); }
    [[nodiscard]] const Error& error() const& { return std::get<Error>(state_); }

private:
    explicit Result(T value) : state_(std::in_place_type<T>, std::move(value)) {}
    explicit Result(Error error) : state_(std::in_place_type<Error>, std::move(error)) {}

    std::variant<T, Error> state_;
};

template <>
class [[nodiscard]] Result<void> final {
public:
    static Result success() { return Result(std::monostate{}); }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<std::monostate>(state_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    void value() const { static_cast<void>(std::get<std::monostate>(state_)); }

    [[nodiscard]] Error& error() & { return std::get<Error>(state_); }
    [[nodiscard]] const Error& error() const& { return std::get<Error>(state_); }

private:
    explicit Result(std::monostate value) : state_(value) {}
    explicit Result(Error error) : state_(std::in_place_type<Error>, std::move(error)) {}

    std::variant<std::monostate, Error> state_;
};

}  // namespace context_reader
