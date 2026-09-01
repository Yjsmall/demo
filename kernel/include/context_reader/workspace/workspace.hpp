#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

#include "context_reader/shared/stable_id.hpp"

namespace context_reader {

struct WorkspaceInfo final {
    WorkspaceId id;
    std::uint32_t schema_version;
};

struct WorkspaceInspection final {
    WorkspaceId id;
    std::uint32_t schema_version;
    std::uint32_t target_schema_version;
    bool migration_required;
};

struct DocumentRecord final {
    DocumentId document_id;
    DocumentVersionId version_id;
    std::string title;
    std::string content_sha256;
    std::string object_key;
    std::uint64_t byte_length;
    std::size_t page_count;
};

struct ImportDocumentResult final {
    DocumentRecord document;
    bool reused_existing;
};

struct WorkspaceVerification final {
    bool valid;
    std::size_t document_count;
    std::size_t document_version_count;
    std::size_t referenced_object_count;
    std::size_t orphaned_object_count;
    std::vector<std::string> issues;
};

struct OrphanCleanupResult final {
    std::size_t removed_object_count;
    std::uint64_t reclaimed_bytes;
};

enum class SearchResultKind : std::uint8_t {
    pdf_page,
    note,
};

struct SearchResultItem final {
    SearchResultKind kind;
    DocumentVersionId document_version_id;
    std::optional<NoteId> note_id;
    std::optional<std::size_t> page_index;
    std::string title;
    std::string excerpt;
};

struct SearchResponse final {
    std::string index_status;
    std::vector<SearchResultItem> results;
};

struct AssetRecord final {
    AssetId id;
    std::string content_sha256;
    std::string media_type;
    std::uint64_t byte_length;
    std::uint32_t width;
    std::uint32_t height;
};

struct AssetData final {
    AssetRecord asset;
    std::vector<std::uint8_t> bytes;
};

struct BackupInspection final {
    bool valid;
    std::uint32_t format_version;
    std::size_t file_count;
    std::uint64_t total_uncompressed_bytes;
    std::vector<std::string> issues;
};

}  // namespace context_reader
