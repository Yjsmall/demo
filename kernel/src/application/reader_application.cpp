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
    std::shared_ptr<DocumentSession> document_session;
#endif
};

ReaderApplication::ReaderApplication() : implementation_(std::make_unique<Impl>()) {}

ReaderApplication::~ReaderApplication() = default;

RuntimeInfo ReaderApplication::runtime_info() const noexcept {
    return RuntimeInfo{
        .version = RuntimeVersion{.major = 0, .minor = 1, .patch = 0},
        .application_api_version = 6,
        .build_id = CONTEXT_READER_BUILD_ID,
        .capabilities = {
            "raw-rgba-tiles",
            "character-text-layout",
            "point-text-selection",
            "workspace-search",
            "note-assets",
            "readerpkg-v1",
            "diagnostics-v1",
        },
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
    auto workspace = std::move(workspace_result).value();
    auto cleanup_result = workspace->cleanup_orphaned_objects();
    if(!cleanup_result) {
        return Result<WorkspaceInfo>::failure(cleanup_result.error());
    }
    implementation_->workspace = std::move(workspace);
    return Result<WorkspaceInfo>::success(implementation_->workspace->info());
#else
    static_cast<void>(root);
    return Result<WorkspaceInfo>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<void> ReaderApplication::close_workspace() {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    std::shared_ptr<DocumentSession> session;
    std::unique_ptr<SqliteWorkspace> workspace;
    {
        const std::scoped_lock lock(implementation_->mutex);
        if(implementation_->workspace == nullptr) {
            return Result<void>::failure(Error(ErrorCode::not_found, "No workspace is open"));
        }
        session = std::exchange(implementation_->document_session, nullptr);
        workspace = std::move(implementation_->workspace);
    }
    if(session) session->shutdown();
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
    const std::filesystem::path& source,
    const CancellationToken& cancellation
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(cancellation.is_cancellation_requested()) {
        return Result<ImportDocumentResult>::failure(
            Error(ErrorCode::cancelled, "Document import was cancelled")
        );
    }
    if(implementation_->workspace == nullptr) {
        return Result<ImportDocumentResult>::failure(
            Error(ErrorCode::not_found, "No workspace is open")
        );
    }
    return implementation_->workspace->import_pdf(source, cancellation);
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

Result<DocumentRecord> ReaderApplication::open_document(
    DocumentId document_id,
    const CancellationToken& cancellation
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(cancellation.is_cancellation_requested()) {
        return Result<DocumentRecord>::failure(
            Error(ErrorCode::cancelled, "Document open was cancelled")
        );
    }
    if(implementation_->workspace == nullptr) {
        return Result<DocumentRecord>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    auto resolved = implementation_->workspace->resolve_document(document_id);
    if(!resolved) {
        return Result<DocumentRecord>::failure(resolved.error());
    }
    if(cancellation.is_cancellation_requested()) {
        return Result<DocumentRecord>::failure(
            Error(ErrorCode::cancelled, "Document open was cancelled")
        );
    }
    auto session = DocumentSession::open(implementation_->pdf_engine, resolved.value().path);
    if(!session) {
        return Result<DocumentRecord>::failure(session.error());
    }
    if(cancellation.is_cancellation_requested()) {
        return Result<DocumentRecord>::failure(
            Error(ErrorCode::cancelled, "Document open was cancelled")
        );
    }
    auto record = std::move(resolved).value().document;
    implementation_->document_session = std::shared_ptr<DocumentSession>(
        std::move(session).value()
    );
    return Result<DocumentRecord>::success(std::move(record));
#else
    static_cast<void>(document_id);
    return Result<DocumentRecord>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<void> ReaderApplication::close_document() {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    std::shared_ptr<DocumentSession> session;
    {
        const std::scoped_lock lock(implementation_->mutex);
        if(implementation_->document_session == nullptr) {
            return Result<void>::failure(Error(ErrorCode::not_found, "No document is open"));
        }
        session = std::exchange(implementation_->document_session, nullptr);
    }
    session->shutdown();
    return Result<void>::success();
#else
    return Result<void>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<PageInfo> ReaderApplication::page_info(std::size_t page_index) {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    std::shared_ptr<DocumentSession> session;
    {
        const std::scoped_lock lock(implementation_->mutex);
        session = implementation_->document_session;
    }
    if(!session) return Result<PageInfo>::failure(Error(ErrorCode::not_found, "No document is open"));
    return session->page_info(page_index);
#else
    static_cast<void>(page_index);
    return Result<PageInfo>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<EncodedPageImage> ReaderApplication::render_page(
    std::size_t page_index,
    double pixels_per_point,
    const CancellationToken& cancellation
) {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(cancellation.is_cancellation_requested()) {
        return Result<EncodedPageImage>::failure(
            Error(ErrorCode::cancelled, "Page render was cancelled")
        );
    }
    std::shared_ptr<DocumentSession> session;
    {
        const std::scoped_lock lock(implementation_->mutex);
        session = implementation_->document_session;
    }
    if(!session) {
        return Result<EncodedPageImage>::failure(
            Error(ErrorCode::not_found, "No document is open")
        );
    }
    auto rendered = session->render_page_png(page_index, pixels_per_point);
    if(cancellation.is_cancellation_requested()) {
        return Result<EncodedPageImage>::failure(
            Error(ErrorCode::cancelled, "Page render was cancelled")
        );
    }
    return rendered;
#else
    static_cast<void>(page_index);
    static_cast<void>(pixels_per_point);
    return Result<EncodedPageImage>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<PageText> ReaderApplication::extract_page_text(std::size_t page_index) {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    std::shared_ptr<DocumentSession> session;
    {
        const std::scoped_lock lock(implementation_->mutex);
        session = implementation_->document_session;
    }
    if(!session) return Result<PageText>::failure(Error(ErrorCode::not_found, "No document is open"));
    return session->extract_text(page_index);
#else
    static_cast<void>(page_index);
    return Result<PageText>::failure(
        Error(ErrorCode::unsupported_document, "Workspace support is not available")
    );
#endif
}

Result<RenderedTile> ReaderApplication::render_tile(
    const TileRequest& request,
    const CancellationToken& cancellation
) {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(cancellation.is_cancellation_requested()) {
        return Result<RenderedTile>::failure(Error(ErrorCode::cancelled, "Tile render was cancelled"));
    }
    std::shared_ptr<DocumentSession> session;
    {
        const std::scoped_lock lock(implementation_->mutex);
        session = implementation_->document_session;
    }
    if(!session) {
        return Result<RenderedTile>::failure(Error(ErrorCode::not_found, "No document is open"));
    }
    auto rendered = session->render_tile(request);
    if(cancellation.is_cancellation_requested()) {
        return Result<RenderedTile>::failure(Error(ErrorCode::cancelled, "Tile render was cancelled"));
    }
    return rendered;
#else
    static_cast<void>(request);
    return Result<RenderedTile>::failure(
        Error(ErrorCode::unsupported_document, "Tile rendering is not available")
    );
#endif
}

Result<PageTextLayout> ReaderApplication::page_text_layout(std::size_t page_index) {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    std::shared_ptr<DocumentSession> session;
    {
        const std::scoped_lock lock(implementation_->mutex);
        session = implementation_->document_session;
    }
    if(!session) return Result<PageTextLayout>::failure(Error(ErrorCode::not_found, "No document is open"));
    return session->page_text_layout(page_index);
#else
    static_cast<void>(page_index);
    return Result<PageTextLayout>::failure(
        Error(ErrorCode::unsupported_document, "Structured text layout is not available")
    );
#endif
}

Result<TextSelection> ReaderApplication::select_text(
    std::size_t page_index,
    PagePoint start_point,
    PagePoint end_point
) {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    std::shared_ptr<DocumentSession> session;
    {
        const std::scoped_lock lock(implementation_->mutex);
        session = implementation_->document_session;
    }
    if(!session) return Result<TextSelection>::failure(Error(ErrorCode::not_found, "No document is open"));
    return session->select_text(page_index, start_point, end_point);
#else
    static_cast<void>(page_index);
    static_cast<void>(start_point);
    static_cast<void>(end_point);
    return Result<TextSelection>::failure(
        Error(ErrorCode::unsupported_document, "Point selection is not available")
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

Result<void> ReaderApplication::rebuild_search_index(const CancellationToken& cancellation) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<void>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->rebuild_search_index(cancellation);
#else
    static_cast<void>(cancellation);
    return Result<void>::failure(
        Error(ErrorCode::unsupported_document, "Workspace search is not available")
    );
#endif
}

Result<SearchResponse> ReaderApplication::search(std::string_view query, std::size_t limit) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<SearchResponse>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->search(query, limit);
#else
    static_cast<void>(query);
    static_cast<void>(limit);
    return Result<SearchResponse>::failure(
        Error(ErrorCode::unsupported_document, "Workspace search is not available")
    );
#endif
}

Result<AssetRecord> ReaderApplication::import_note_asset(
    AnnotationId annotation_id,
    const std::filesystem::path& source,
    const CancellationToken& cancellation
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<AssetRecord>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->import_note_asset(annotation_id, source, cancellation);
#else
    static_cast<void>(annotation_id);
    static_cast<void>(source);
    static_cast<void>(cancellation);
    return Result<AssetRecord>::failure(
        Error(ErrorCode::unsupported_document, "Note assets are not available")
    );
#endif
}

Result<AssetData> ReaderApplication::read_asset(AssetId asset_id) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<AssetData>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->read_asset(asset_id);
#else
    static_cast<void>(asset_id);
    return Result<AssetData>::failure(
        Error(ErrorCode::unsupported_document, "Note assets are not available")
    );
#endif
}

Result<BackupInspection> ReaderApplication::export_workspace(
    const std::filesystem::path& destination,
    const CancellationToken& cancellation
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace == nullptr) {
        return Result<BackupInspection>::failure(Error(ErrorCode::not_found, "No workspace is open"));
    }
    return implementation_->workspace->export_package(destination, cancellation);
#else
    static_cast<void>(destination);
    static_cast<void>(cancellation);
    return Result<BackupInspection>::failure(Error(ErrorCode::unsupported_document, "Workspace backup is not available"));
#endif
}

Result<BackupInspection> ReaderApplication::inspect_backup(
    const std::filesystem::path& package_path
) {
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    return SqliteWorkspace::inspect_package(package_path);
#else
    static_cast<void>(package_path);
    return Result<BackupInspection>::failure(Error(ErrorCode::unsupported_document, "Workspace backup is not available"));
#endif
}

Result<WorkspaceInfo> ReaderApplication::restore_workspace(
    const std::filesystem::path& package_path,
    const std::filesystem::path& empty_target,
    const CancellationToken& cancellation
) {
    const std::scoped_lock lock(implementation_->mutex);
#if defined(CONTEXT_READER_HAS_MUPDF) && defined(CONTEXT_READER_HAS_WORKSPACE)
    if(implementation_->workspace != nullptr) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::conflict, "Close the current workspace before restoring"));
    }
    if(cancellation.is_cancellation_requested()) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::cancelled, "Workspace restore was cancelled"));
    }
    auto restored = SqliteWorkspace::restore_package(package_path, empty_target, cancellation);
    if(!restored) return restored;
    auto opened = SqliteWorkspace::open(empty_target, implementation_->pdf_engine);
    if(!opened) return Result<WorkspaceInfo>::failure(opened.error());
    implementation_->workspace = std::move(opened).value();
    return Result<WorkspaceInfo>::success(implementation_->workspace->info());
#else
    static_cast<void>(package_path);
    static_cast<void>(empty_target);
    static_cast<void>(cancellation);
    return Result<WorkspaceInfo>::failure(Error(ErrorCode::unsupported_document, "Workspace backup is not available"));
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
