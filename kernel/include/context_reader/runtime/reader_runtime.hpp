#pragma once

#include <memory>

#include "context_reader/application/reader_application.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

class ReaderRuntime final {
public:
    [[nodiscard]] static Result<std::unique_ptr<ReaderRuntime>> create();

    ReaderRuntime(const ReaderRuntime&) = delete;
    ReaderRuntime& operator=(const ReaderRuntime&) = delete;
    ReaderRuntime(ReaderRuntime&&) = delete;
    ReaderRuntime& operator=(ReaderRuntime&&) = delete;
    ~ReaderRuntime() = default;

    [[nodiscard]] ReaderApplication& application() noexcept { return application_; }
    [[nodiscard]] const ReaderApplication& application() const noexcept { return application_; }

private:
    ReaderRuntime() = default;

    ReaderApplication application_;
};

}  // namespace context_reader
