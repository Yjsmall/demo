#pragma once

#include <filesystem>
#include <memory>

#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/result.hpp"
#include "context_reader/workspace/workspace.hpp"

namespace context_reader {

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
        const std::filesystem::path& source
    );
    [[nodiscard]] Result<std::vector<DocumentRecord>> list_documents();
    [[nodiscard]] Result<WorkspaceVerification> verify();

private:
    class Impl;

    explicit SqliteWorkspace(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

}  // namespace context_reader
