#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace context_reader {

enum class ErrorCode : std::uint8_t {
    invalid_argument,
    not_found,
    already_exists,
    conflict,
    workspace_busy,
    unsupported_document,
    password_required,
    cancelled,
    resource_exhausted,
    storage_failure,
    pdf_failure,
    internal,
};

class Error final {
public:
    Error(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    ErrorCode code_;
    std::string message_;
};

}  // namespace context_reader
