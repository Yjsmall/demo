#pragma once

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace context_reader {

template <typename Tag>
class StableId final {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    [[nodiscard]] static constexpr StableId from_bytes(Bytes bytes) noexcept {
        return StableId(bytes);
    }

    [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

    [[nodiscard]] constexpr bool is_nil() const noexcept {
        for(const auto byte : bytes_) {
            if(byte != 0U) {
                return false;
            }
        }
        return true;
    }

    auto operator<=>(const StableId&) const = default;

private:
    explicit constexpr StableId(Bytes bytes) noexcept : bytes_(bytes) {}

    Bytes bytes_;
};

struct WorkspaceIdTag final {};
struct DocumentIdTag final {};
struct DocumentVersionIdTag final {};
struct AnnotationIdTag final {};
struct NoteIdTag final {};
struct AssetIdTag final {};
struct JobIdTag final {};

using WorkspaceId = StableId<WorkspaceIdTag>;
using DocumentId = StableId<DocumentIdTag>;
using DocumentVersionId = StableId<DocumentVersionIdTag>;
using AnnotationId = StableId<AnnotationIdTag>;
using NoteId = StableId<NoteIdTag>;
using AssetId = StableId<AssetIdTag>;
using JobId = StableId<JobIdTag>;

template <typename Tag>
[[nodiscard]] std::string stable_id_to_hex(const StableId<Tag>& id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for(std::size_t index = 0; index < id.bytes().size(); ++index) {
        const auto byte = id.bytes()[index];
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 0x0FU];
    }
    return result;
}

template <typename Tag>
[[nodiscard]] std::optional<StableId<Tag>> stable_id_from_hex(std::string_view value) {
    if(value.size() != 32U) {
        return std::nullopt;
    }
    typename StableId<Tag>::Bytes bytes{};
    const auto nibble = [](char character) -> std::optional<std::uint8_t> {
        if(character >= '0' && character <= '9') {
            return static_cast<std::uint8_t>(character - '0');
        }
        if(character >= 'a' && character <= 'f') {
            return static_cast<std::uint8_t>(character - 'a' + 10);
        }
        if(character >= 'A' && character <= 'F') {
            return static_cast<std::uint8_t>(character - 'A' + 10);
        }
        return std::nullopt;
    };
    for(std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = nibble(value[index * 2]);
        const auto low = nibble(value[index * 2 + 1]);
        if(!high || !low) {
            return std::nullopt;
        }
        bytes[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return StableId<Tag>::from_bytes(bytes);
}

}  // namespace context_reader
