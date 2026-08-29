#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "context_reader/pdf/page_geometry.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

struct PageInfo final {
    std::size_t index;
    PageSize size;
    PageRotation rotation;

    bool operator==(const PageInfo&) const = default;
};

struct EncodedPageImage final {
    std::size_t width_pixels;
    std::size_t height_pixels;
    double pixels_per_point;
    std::vector<std::uint8_t> png;
};

struct TextLineInfo final {
    std::string text;
    PageRect bounds;
    bool vertical;
};

struct PageText final {
    std::string text;
    std::vector<TextLineInfo> lines;
};

class PdfDocument {
public:
    PdfDocument() = default;
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;
    virtual ~PdfDocument() = default;

    [[nodiscard]] virtual std::size_t page_count() const noexcept = 0;
    [[nodiscard]] virtual Result<PageInfo> page_info(std::size_t page_index) = 0;
    [[nodiscard]] virtual Result<EncodedPageImage> render_page_png(
        std::size_t page_index,
        double pixels_per_point
    ) = 0;
    [[nodiscard]] virtual Result<PageText> extract_text(std::size_t page_index) = 0;
};

class PdfEngine {
public:
    PdfEngine() = default;
    PdfEngine(const PdfEngine&) = delete;
    PdfEngine& operator=(const PdfEngine&) = delete;
    virtual ~PdfEngine() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<PdfDocument>> open(
        const std::filesystem::path& source
    ) = 0;
};

}  // namespace context_reader
