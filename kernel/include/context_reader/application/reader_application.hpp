#pragma once

#include <cstdint>

namespace context_reader {

struct RuntimeVersion final {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;

    bool operator==(const RuntimeVersion&) const = default;
};

struct RuntimeInfo final {
    RuntimeVersion version;
    std::uint32_t application_api_version;

    bool operator==(const RuntimeInfo&) const = default;
};

class ReaderApplication final {
public:
    [[nodiscard]] RuntimeInfo runtime_info() const noexcept;
};

}  // namespace context_reader
