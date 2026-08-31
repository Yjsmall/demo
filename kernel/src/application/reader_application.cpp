#include "context_reader/application/reader_application.hpp"

#include <memory>
#include <mutex>
#include <utility>

#include "context_reader/shared/error.hpp"

#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
#include "context_reader/pdf/document_session.hpp"
#include "context_reader/pdf/mupdf_engine.hpp"
#include "context_reader/workspace/sqlite_workspace.hpp"
#endif

namespace context_reader {

class ReaderApplication::Impl final {
public:
    mutable std::mutex mutex;
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    MuPdfEngine pdf_engine;
    std::unique_ptr<SqliteWorkspace> workspace;
    std::unique_ptr<DocumentSession> document_session;
#endif
};

ReaderApplication::ReaderApplication() : implementation_(std::make_unique<Impl>()) {}

ReaderApplication::~ReaderApplication() = default;

RuntimeInfo ReaderApplication::runtime_info() const noexcept {
    return RuntimeInfo{
        .version = RuntimeVersion{.major = 0, .minor = 1, .patch = 0},
        .application_api_version = 4,
    };
}

Result<WorkspaceInfo> ReaderApplication::create_workspace(const std::filesystem::path& root) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace != nullptr) {
        return Result<WorkspaceInfo>::failure(
            Error(ErrorCode::conflict, "A workspace is already open")
        );
    }
    auto workspace_result = SqliteWorkspace::create(root, implementation_->pdf_engine);
    if(!workspace_result) {
        return Result<WorkspaceInfo>::failure(workspace_result.error());
    }
    implementation_->workspace = std::move(workspace_result).value();
    return Result<WorkspaceInfo>::success(implementation_->workspace->info());
#else
    static_cast<void>(root);
    return Result<WorkspaceInfo>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<WorkspaceInfo> ReaderApplication::open_workspace(const std::filesystem::path& root) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace != nullptr) {
        return Result<WorkspaceInfo>::failure(
            Error(ErrorCode::conflict, "A workspace is already open")
        );
    }
    auto workspace_result = SqliteWorkspace::open(root, implementation_->pdf_engine);
    if(!workspace_result) {
        return Result<WorkspaceInfo>::failure(workspace_result.error());
    }
    implementation_->workspace = std::move(workspace_result).value();
    return Result<WorkspaceInfo>::success(implementation_->workspace->info());
#else
    static_cast<void>(root);
    return Result<WorkspaceInfo>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<void> ReaderApplication::close_workspace() {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<void>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    implementation_->document_session.reset();
    implementation_->workspace.reset();
    return Result<void>::success();
#else
    return Result<void>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<WorkspaceInfo> ReaderApplication::workspace_info() const {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return Result<WorkspaceInfo>::success(implementation_->workspace->info());
#else
    return Result<WorkspaceInfo>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<ImportDocumentResult> ReaderApplication::import_document(
    const std::filesystem::path& source
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<ImportDocumentResult>::failure(
            Error(ErrorCode::not_found, "No workspace is open")
        );
    }
    return implementation_->workspace->import_pdf(source);
#else
    static_cast<void>(source);
    return Result<ImportDocumentResult>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<std::vector<DocumentRecord>> ReaderApplication::list_documents() {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<std::vector<DocumentRecord>>::failure(
            Error(ErrorCode::not_found, "No workspace is open")
        );
    }
    return implementation_->workspace->list_documents();
#else
    return Result<std::vector<DocumentRecord>>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<DocumentRecord> ReaderApplication::open_document(DocumentId document_id) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<DocumentRecord>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    auto resolved = implementation_->workspace->resolve_document(document_id);
    if(!resolved) {
        return Result<DocumentRecord>::failure(resolved.error());
    }
    auto session = DocumentSession::open(implementation_->pdf_engine, resolved.value().path);
    if(!session) {
        return Result<DocumentRecord>::failure(session.error());
    }
    auto record = std::move(resolved).value().document;
    implementation_->document_session = std::move(session).value();
    return Result<DocumentRecord>::success(std::move(record));
#else
    static_cast<void>(document_id);
    return Result<DocumentRecord>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<void> ReaderApplication::close_document() {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->document_session == nullptr) {
        return Result<void>::failure(Error(ErrorCode::not_found, "No document is open"));
    }
    implementation_->document_session.reset();
    return Result<void>::success();
#else
    return Result<void>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<PageInfo> ReaderApplication::page_info(std::size_t page_index) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->document_session == nullptr) {
        return Result<PageInfo>::failure(Error(ErrorCode::not_found, "No document is open"));
    }
    return implementation_->document_session->page_info(page_index);
#else
    static_cast<void>(page_index);
    return Result<PageInfo>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<EncodedPageImage> ReaderApplication::render_page(
    std::size_t page_index,
    double pixels_per_point
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->document_session == nullptr) {
        return Result<EncodedPageImage>::failure(
            Error(ErrorCode::not_found, "No document is open")
        );
    }
    return implementation_->document_session->render_page_png(page_index, pixels_per_point);
#else
    static_cast<void>(page_index);
    static_cast<void>(pixels_per_point);
    return Result<EncodedPageImage>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<PageText> ReaderApplication::extract_page_text(std::size_t page_index) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->document_session == nullptr) {
        return Result<PageText>::failure(Error(ErrorCode::not_found, "No document is open"));
    }
    return implementation_->document_session->extract_text(page_index);
#else
    static_cast<void>(page_index);
    return Result<PageText>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<AnnotationRecord> ReaderApplication::create_annotation(const CreateAnnotation& command) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<AnnotationRecord>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->create_annotation(command);
#else
    static_cast<void>(command);
    return Result<AnnotationRecord>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<std::vector<AnnotationRecord>> ReaderApplication::list_annotations(
    DocumentVersionId document_version_id
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<std::vector<AnnotationRecord>>::failure(
            Error(ErrorCode::not_found, "No workspace is open")
        );
    }
    return implementation_->workspace->list_annotations(document_version_id);
#else
    static_cast<void>(document_version_id);
    return Result<std::vector<AnnotationRecord>>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<void> ReaderApplication::delete_annotation(AnnotationId annotation_id) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<void>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->delete_annotation(annotation_id);
#else
    static_cast<void>(annotation_id);
    return Result<void>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<NoteRecord> ReaderApplication::update_note(const UpdateNote& command) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<NoteRecord>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->update_note(command);
#else
    static_cast<void>(command);
    return Result<NoteRecord>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<std::vector<NoteRecord>> ReaderApplication::list_notes(
    DocumentVersionId document_version_id
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<std::vector<NoteRecord>>::failure(
            Error(ErrorCode::not_found, "No workspace is open")
        );
    }
    return implementation_->workspace->list_notes(document_version_id);
#else
    static_cast<void>(document_version_id);
    return Result<std::vector<NoteRecord>>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<WorkspaceVerification> ReaderApplication::verify_workspace() {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<WorkspaceVerification>::failure(
            Error(ErrorCode::not_found, "No workspace is open")
        );
    }
    return implementation_->workspace->verify();
#else
    return Result<WorkspaceVerification>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

}  // namespace context_reader
