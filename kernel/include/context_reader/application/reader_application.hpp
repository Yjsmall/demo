#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "context_reader/pdf/pdf_engine.hpp"
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
        const std::filesystem::path& source
    );
    [[nodiscard]] Result<std::vector<DocumentRecord>> list_documents();
    [[nodiscard]] Result<DocumentRecord> open_document(DocumentId document_id);
    [[nodiscard]] Result<void> close_document();
    [[nodiscard]] Result<PageInfo> page_info(std::size_t page_index);
    [[nodiscard]] Result<EncodedPageImage> render_page(
        std::size_t page_index,
        double pixels_per_point
    );
    [[nodiscard]] Result<PageText> extract_page_text(std::size_t page_index);
    [[nodiscard]] Result<WorkspaceVerification> verify_workspace();

private:
    class Impl;

    std::unique_ptr<Impl> implementation_;
};

}  // namespace context_reader
