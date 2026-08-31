#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

}  // namespace context_reader
