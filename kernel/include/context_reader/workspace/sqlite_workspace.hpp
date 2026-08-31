#pragma once

#include <filesystem>
#include <memory>
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
    [[nodiscard]] Result<WorkspaceVerification> verify();

private:
    class Impl;

    explicit SqliteWorkspace(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

}  // namespace context_reader
