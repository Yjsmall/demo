#include "context_reader/pdf/document_session.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace context_reader {

Result<std::unique_ptr<DocumentSession>> DocumentSession::open(
    PdfEngine& engine,
    const std::filesystem::path& source
) {
    auto document_result = engine.open(source);
    if(!document_result) {
        return Result<std::unique_ptr<DocumentSession>>::failure(document_result.error());
    }
    return Result<std::unique_ptr<DocumentSession>>::success(
        std::unique_ptr<DocumentSession>(
            new DocumentSession(std::move(document_result).value())
        )
    );
}

DocumentSession::DocumentSession(std::unique_ptr<PdfDocument> document) noexcept
    : document_(std::move(document)) {}

std::size_t DocumentSession::page_count() const noexcept {
    const std::scoped_lock lock(mutex_);
    return document_->page_count();
}

Result<PageInfo> DocumentSession::page_info(std::size_t page_index) {
    const std::scoped_lock lock(mutex_);
    return document_->page_info(page_index);
}

Result<EncodedPageImage> DocumentSession::render_page_png(
    std::size_t page_index,
    double pixels_per_point
) {
    const std::scoped_lock lock(mutex_);
    return document_->render_page_png(page_index, pixels_per_point);
}

Result<PageText> DocumentSession::extract_text(std::size_t page_index) {
    const std::scoped_lock lock(mutex_);
    return document_->extract_text(page_index);
}

}  // namespace context_reader
