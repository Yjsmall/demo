#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "context_reader/pdf/mupdf_engine.hpp"
#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/error.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if(!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    using namespace context_reader;

    const std::filesystem::path corpus_root(CONTEXT_READER_TEST_CORPUS);
    MuPdfEngine engine;

    check(std::string_view(MuPdfEngine::version()) == "1.28.3", "adapter pins MuPDF 1.28.3");

    const auto missing = engine.open(corpus_root / "generated" / "missing.pdf");
    check(!missing.has_value(), "missing PDF is rejected");
    if(!missing) {
        check(missing.error().code() == ErrorCode::not_found, "missing PDF has stable error code");
    }

    const auto corrupt = engine.open(corpus_root / "generated" / "corrupt-truncated.pdf");
    check(!corrupt.has_value(), "corrupt PDF is rejected");
    if(!corrupt) {
        check(corrupt.error().code() == ErrorCode::pdf_failure, "corrupt PDF has stable error code");
    }

    auto opened = engine.open(corpus_root / "generated" / "basic-rotated-cropbox.pdf");
    check(opened.has_value(), "generated PDF opens through MuPDF");
    if(opened) {
        auto document = std::move(opened).value();
        check(document->page_count() == 1U, "MuPDF reports page count");

        const auto page = document->page_info(0U);
        check(page.has_value(), "MuPDF reports page metadata");
        if(page) {
            check(page.value().size.width == 540.0, "CropBox width is reported in points");
            check(page.value().size.height == 648.0, "CropBox height is reported in points");
            check(
                page.value().rotation == PageRotation::degrees_90,
                "inherited page rotation is normalized"
            );
        }

        const auto rendered = document->render_page_png(0U, 1.0);
        check(rendered.has_value(), "MuPDF renders a page to PNG");
        if(rendered) {
            check(rendered.value().width_pixels == 648U, "rotation swaps rendered width");
            check(rendered.value().height_pixels == 540U, "rotation swaps rendered height");
            const auto& png = rendered.value().png;
            check(png.size() > 8U, "rendered PNG is not empty");
            if(png.size() >= 8U) {
                const std::uint8_t expected_signature[] = {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
                check(
                    std::equal(std::begin(expected_signature), std::end(expected_signature), png.begin()),
                    "rendered bytes have a PNG signature"
                );
            }
        }

        const auto invalid_scale = document->render_page_png(0U, 0.0);
        check(!invalid_scale.has_value(), "invalid render scale is rejected");
        if(!invalid_scale) {
            check(
                invalid_scale.error().code() == ErrorCode::invalid_argument,
                "invalid render scale has stable error code"
            );
        }

        const auto extracted = document->extract_text(0U);
        check(extracted.has_value(), "MuPDF extracts structured text");
        if(extracted) {
            check(
                extracted.value().text.find("Context Reader P1") != std::string::npos,
                "plain text contains fixture content"
            );
            check(extracted.value().lines.size() == 1U, "fixture has one text line");
            if(!extracted.value().lines.empty()) {
                const auto& line = extracted.value().lines.front();
                check(line.text == "Context Reader P1", "line text is UTF-8");
                check(line.bounds.width > 0.0 && line.bounds.height > 0.0, "line bounds are non-empty");
                check(
                    line.bounds.x >= 0.0 && line.bounds.y >= 0.0 &&
                        line.bounds.x + line.bounds.width <= 540.0 &&
                        line.bounds.y + line.bounds.height <= 648.0,
                    "line bounds use normalized CropBox page coordinates"
                );
            }
        }

        const auto missing_page = document->page_info(1U);
        check(!missing_page.has_value(), "out-of-range page is rejected");
        if(!missing_page) {
            check(
                missing_page.error().code() == ErrorCode::not_found,
                "out-of-range page has stable error code"
            );
        }
    }

    auto cjk = engine.open(corpus_root / "generated" / "cjk-to-unicode.pdf");
    check(cjk.has_value(), "generated CJK PDF opens through MuPDF");
    if(cjk) {
        auto document = std::move(cjk).value();
        const auto extracted = document->extract_text(0U);
        check(extracted.has_value(), "CJK text extraction succeeds");
        if(extracted) {
            check(
                extracted.value().text.find("\xE8\xAF\xAD\xE5\xA2\x83\xE9\x98\x85\xE8\xAF\xBB") !=
                    std::string::npos,
                "ToUnicode mapping preserves CJK text"
            );
        }
    }

    auto columns = engine.open(corpus_root / "generated" / "double-column.pdf");
    check(columns.has_value(), "generated double-column PDF opens through MuPDF");
    if(columns) {
        auto document = std::move(columns).value();
        const auto extracted = document->extract_text(0U);
        check(extracted.has_value(), "double-column text extraction succeeds");
        if(extracted) {
            const auto& text = extracted.value().text;
            const auto left_first = text.find("Left column line 1");
            const auto left_second = text.find("Left column line 2");
            const auto right_first = text.find("Right column line 1");
            const auto right_second = text.find("Right column line 2");
            check(
                left_first < left_second && left_second < right_first && right_first < right_second,
                "double-column fixture has deterministic reading order"
            );
            check(extracted.value().lines.size() == 4U, "double-column fixture has four lines");
        }
    }

    auto scan = engine.open(corpus_root / "generated" / "image-only-scan.pdf");
    check(scan.has_value(), "generated image-only PDF opens through MuPDF");
    if(scan) {
        auto document = std::move(scan).value();
        const auto extracted = document->extract_text(0U);
        check(extracted.has_value(), "image-only text extraction returns a supported result");
        if(extracted) {
            check(extracted.value().text.empty(), "image-only page has no synthetic text");
            check(extracted.value().lines.empty(), "image-only page has no text lines");
        }
        const auto rendered = document->render_page_png(0U, 1.0);
        check(rendered.has_value(), "image-only page renders successfully");
        if(rendered) {
            check(
                rendered.value().width_pixels == 400U && rendered.value().height_pixels == 500U,
                "image-only render uses page dimensions"
            );
        }
    }

    if(failures == 0) {
        std::cout << "mupdf_engine_test passed\n";
    }
    return failures == 0 ? 0 : 1;
}
