#pragma once

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "context_reader/annotation/annotation.hpp"
#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/runtime/cancellation.hpp"
#include "context_reader/shared/result.hpp"
#include "context_reader/workspace/workspace.hpp"

namespace context_reader {

struct ResolvedDocumentObject final {
    DocumentRecord document;
    std::filesystem::path path;
};

class SqliteWorkspace final {
public:
    [[nodiscard]] static Result<std::unique_ptr<SqliteWorkspace>> create(
        const std::filesystem::path& root,
        PdfEngine& pdf_engine
    );
    [[nodiscard]] static Result<std::unique_ptr<SqliteWorkspace>> open(
        const std::filesystem::path& root,
        PdfEngine& pdf_engine
    );
    [[nodiscard]] static Result<WorkspaceInspection> inspect(
        const std::filesystem::path& root
    );
    [[nodiscard]] static Result<BackupInspection> inspect_package(
        const std::filesystem::path& package_path
    );
    [[nodiscard]] static Result<WorkspaceInfo> restore_package(
        const std::filesystem::path& package_path,
        const std::filesystem::path& empty_target,
        const CancellationToken& cancellation = CancellationToken{}
    );

    SqliteWorkspace(const SqliteWorkspace&) = delete;
    SqliteWorkspace& operator=(const SqliteWorkspace&) = delete;
    SqliteWorkspace(SqliteWorkspace&&) = delete;
    SqliteWorkspace& operator=(SqliteWorkspace&&) = delete;
    ~SqliteWorkspace();

    [[nodiscard]] WorkspaceInfo info() const noexcept;
    [[nodiscard]] Result<ImportDocumentResult> import_pdf(
        const std::filesystem::path& source,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<std::vector<DocumentRecord>> list_documents();
    [[nodiscard]] Result<ResolvedDocumentObject> resolve_document(DocumentId document_id);
    [[nodiscard]] Result<AnnotationRecord> create_annotation(const CreateAnnotation& command);
    [[nodiscard]] Result<std::vector<AnnotationRecord>> list_annotations(
        DocumentVersionId document_version_id
    );
    [[nodiscard]] Result<void> delete_annotation(AnnotationId annotation_id);
    [[nodiscard]] Result<NoteRecord> update_note(const UpdateNote& command);
    [[nodiscard]] Result<std::vector<NoteRecord>> list_notes(
        DocumentVersionId document_version_id
    );
    [[nodiscard]] Result<void> rebuild_search_index(
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<SearchResponse> search(std::string_view query, std::size_t limit);
    [[nodiscard]] Result<AssetRecord> import_note_asset(
        AnnotationId annotation_id,
        const std::filesystem::path& source,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<AssetData> read_asset(AssetId asset_id);
    [[nodiscard]] Result<BackupInspection> export_package(
        const std::filesystem::path& destination,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<WorkspaceVerification> verify();
    [[nodiscard]] Result<OrphanCleanupResult> cleanup_orphaned_objects();

private:
    class Impl;

    explicit SqliteWorkspace(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

}  // namespace context_reader
