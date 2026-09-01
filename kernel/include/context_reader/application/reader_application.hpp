#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "context_reader/annotation/annotation.hpp"
#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/runtime/cancellation.hpp"
#include "context_reader/shared/result.hpp"
#include "context_reader/workspace/workspace.hpp"

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
    std::string build_id;
    std::vector<std::string> capabilities;

    bool operator==(const RuntimeInfo&) const = default;
};

class ReaderApplication final {
public:
    ReaderApplication();
    ReaderApplication(const ReaderApplication&) = delete;
    ReaderApplication& operator=(const ReaderApplication&) = delete;
    ReaderApplication(ReaderApplication&&) = delete;
    ReaderApplication& operator=(ReaderApplication&&) = delete;
    ~ReaderApplication();

    [[nodiscard]] RuntimeInfo runtime_info() const noexcept;

    [[nodiscard]] Result<WorkspaceInfo> create_workspace(
        const std::filesystem::path& root
    );
    [[nodiscard]] Result<WorkspaceInfo> open_workspace(
        const std::filesystem::path& root
    );
    [[nodiscard]] Result<void> close_workspace();
    [[nodiscard]] Result<WorkspaceInfo> workspace_info() const;
    [[nodiscard]] Result<ImportDocumentResult> import_document(
        const std::filesystem::path& source,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<std::vector<DocumentRecord>> list_documents();
    [[nodiscard]] Result<DocumentRecord> open_document(
        DocumentId document_id,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<void> close_document();
    [[nodiscard]] Result<PageInfo> page_info(std::size_t page_index);
    [[nodiscard]] Result<EncodedPageImage> render_page(
        std::size_t page_index,
        double pixels_per_point,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<RenderedTile> render_tile(
        const TileRequest& request,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<PageText> extract_page_text(std::size_t page_index);
    [[nodiscard]] Result<PageTextLayout> page_text_layout(std::size_t page_index);
    [[nodiscard]] Result<TextSelection> select_text(
        std::size_t page_index,
        PagePoint start_point,
        PagePoint end_point
    );
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
    [[nodiscard]] Result<SearchResponse> search(std::string_view query, std::size_t limit = 50);
    [[nodiscard]] Result<AssetRecord> import_note_asset(
        AnnotationId annotation_id,
        const std::filesystem::path& source,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<AssetData> read_asset(AssetId asset_id);
    [[nodiscard]] Result<BackupInspection> export_workspace(
        const std::filesystem::path& destination,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<BackupInspection> inspect_backup(
        const std::filesystem::path& package_path
    );
    [[nodiscard]] Result<WorkspaceInfo> restore_workspace(
        const std::filesystem::path& package_path,
        const std::filesystem::path& empty_target,
        const CancellationToken& cancellation = CancellationToken{}
    );
    [[nodiscard]] Result<WorkspaceVerification> verify_workspace();

private:
    class Impl;

    std::unique_ptr<Impl> implementation_;
};

}  // namespace context_reader
