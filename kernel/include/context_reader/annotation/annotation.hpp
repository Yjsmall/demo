#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "context_reader/pdf/page_geometry.hpp"
#include "context_reader/shared/stable_id.hpp"

namespace context_reader {

enum class HighlightColor : std::uint8_t {
    yellow,
    green,
    blue,
    pink,
};

struct QuoteAnchor final {
    std::string exact;
    std::string prefix;
    std::string suffix;
};

struct CreateAnnotation final {
    DocumentVersionId document_version_id;
    std::size_t page_index;
    std::vector<PageRect> quads;
    QuoteAnchor quote;
    std::string layout_version;
    HighlightColor color;
};

struct AnnotationRecord final {
    AnnotationId id;
    DocumentVersionId document_version_id;
    std::size_t page_index;
    std::vector<PageRect> quads;
    QuoteAnchor quote;
    std::string layout_version;
    HighlightColor color;
};

struct UpdateNote final {
    AnnotationId annotation_id;
    std::uint64_t expected_revision;
    std::string markdown_source;
};

struct NoteRecord final {
    NoteId id;
    AnnotationId annotation_id;
    std::string markdown_source;
    std::uint64_t revision;
};

}  // namespace context_reader
