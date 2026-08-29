#pragma once

#include <array>
#include <compare>
#include <cstdint>

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

}  // namespace context_reader
