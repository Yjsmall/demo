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

struct PageQuad final {
    PagePoint upper_left;
    PagePoint upper_right;
    PagePoint lower_left;
    PagePoint lower_right;

    bool operator==(const PageQuad&) const = default;
};

struct TileRequest final {
    std::size_t page_index;
    double pixels_per_point;
    std::size_t x_pixels;
    std::size_t y_pixels;
    std::size_t width_pixels;
    std::size_t height_pixels;
    std::uint64_t generation;
};

struct RenderedTile final {
    std::size_t page_index;
    std::size_t x_pixels;
    std::size_t y_pixels;
    std::size_t width_pixels;
    std::size_t height_pixels;
    double pixels_per_point;
    std::uint64_t generation;
    std::vector<std::uint8_t> rgba;
};

enum class TextDirection : std::uint8_t {
    left_to_right,
    right_to_left,
    top_to_bottom,
};

struct TextSelectionUnit final {
    std::size_t logical_start;
    std::size_t logical_end;
    std::string text;
    TextDirection direction;
    std::size_t line_index;
    PageQuad quad;
};

struct TextLineInfo final {
    std::string text;
    PageRect bounds;
    bool vertical;
};

struct PageTextLayout final {
    std::size_t page_index;
    std::uint64_t layout_version;
    std::string text;
    std::vector<TextSelectionUnit> units;
    std::vector<TextLineInfo> lines;
};

struct TextSelection final {
    std::size_t page_index;
    std::uint64_t layout_version;
    std::size_t logical_start;
    std::size_t logical_end;
    TextDirection direction;
    std::string text;
    std::vector<PageQuad> quads;
    std::string quote_prefix;
    std::string quote_suffix;
};

struct PageText final {
    std::string text;
    std::vector<TextLineInfo> lines;
};

class PdfTileDisplayList {
public:
    PdfTileDisplayList() = default;
    PdfTileDisplayList(const PdfTileDisplayList&) = delete;
    PdfTileDisplayList& operator=(const PdfTileDisplayList&) = delete;
    virtual ~PdfTileDisplayList() = default;

    [[nodiscard]] virtual Result<RenderedTile> render(const TileRequest& request) = 0;
    [[nodiscard]] virtual std::size_t estimated_bytes() const noexcept = 0;
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
    [[nodiscard]] virtual Result<RenderedTile> render_tile(const TileRequest&) {
        return Result<RenderedTile>::failure(
            Error(ErrorCode::unsupported_document, "Tile rendering is not supported")
        );
    }
    [[nodiscard]] virtual Result<std::shared_ptr<PdfTileDisplayList>> create_tile_display_list(
        std::size_t
    ) {
        return Result<std::shared_ptr<PdfTileDisplayList>>::failure(
            Error(ErrorCode::unsupported_document, "Display-list rendering is not supported")
        );
    }
    [[nodiscard]] virtual Result<PageText> extract_text(std::size_t page_index) = 0;
    [[nodiscard]] virtual Result<PageTextLayout> page_text_layout(std::size_t) {
        return Result<PageTextLayout>::failure(
            Error(ErrorCode::unsupported_document, "Structured text layout is not supported")
        );
    }
    [[nodiscard]] virtual Result<TextSelection> select_text(
        std::size_t,
        PagePoint,
        PagePoint
    ) {
        return Result<TextSelection>::failure(
            Error(ErrorCode::unsupported_document, "Point selection is not supported")
        );
    }
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
