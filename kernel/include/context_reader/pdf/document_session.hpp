#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>

#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

class DocumentSession final {
public:
    [[nodiscard]] static Result<std::unique_ptr<DocumentSession>> open(
        PdfEngine& engine,
        const std::filesystem::path& source
    );

    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;
    DocumentSession(DocumentSession&&) = delete;
    DocumentSession& operator=(DocumentSession&&) = delete;
    ~DocumentSession() = default;

    [[nodiscard]] std::size_t page_count() const noexcept;
    [[nodiscard]] Result<PageInfo> page_info(std::size_t page_index);
    [[nodiscard]] Result<EncodedPageImage> render_page_png(
        std::size_t page_index,
        double pixels_per_point
    );
    [[nodiscard]] Result<PageText> extract_text(std::size_t page_index);

private:
    explicit DocumentSession(std::unique_ptr<PdfDocument> document) noexcept;

    mutable std::mutex mutex_;
    std::unique_ptr<PdfDocument> document_;
};

}  // namespace context_reader
