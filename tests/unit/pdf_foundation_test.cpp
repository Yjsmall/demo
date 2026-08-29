#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>

#include "context_reader/pdf/document_session.hpp"
#include "context_reader/pdf/page_geometry.hpp"
#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/shared/result.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if(!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool close(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 1.0e-9;
}

class FakePdfDocument final : public context_reader::PdfDocument {
public:
    [[nodiscard]] std::size_t page_count() const noexcept override { return 2U; }

    [[nodiscard]] context_reader::Result<context_reader::PageInfo> page_info(
        std::size_t page_index
    ) override {
        if(page_index >= page_count()) {
            return context_reader::Result<context_reader::PageInfo>::failure(
                context_reader::Error(
                    context_reader::ErrorCode::not_found,
                    "page index is out of range"
                )
            );
        }
        return context_reader::Result<context_reader::PageInfo>::success(
            context_reader::PageInfo{
                .index = page_index,
                .size = context_reader::PageSize{612.0, 792.0},
                .rotation = context_reader::PageRotation::degrees_0,
            }
        );
    }

    [[nodiscard]] context_reader::Result<context_reader::EncodedPageImage> render_page_png(
        std::size_t,
        double
    ) override {
        return context_reader::Result<context_reader::EncodedPageImage>::success(
            context_reader::EncodedPageImage{1U, 1U, 1.0, {}}
        );
    }

    [[nodiscard]] context_reader::Result<context_reader::PageText> extract_text(
        std::size_t
    ) override {
        return context_reader::Result<context_reader::PageText>::success(
            context_reader::PageText{"", {}}
        );
    }
};

class FakePdfEngine final : public context_reader::PdfEngine {
public:
    [[nodiscard]] context_reader::Result<std::unique_ptr<context_reader::PdfDocument>> open(
        const std::filesystem::path& source
    ) override {
        if(source.empty()) {
            return context_reader::Result<std::unique_ptr<context_reader::PdfDocument>>::failure(
                context_reader::Error(
                    context_reader::ErrorCode::invalid_argument,
                    "source path is empty"
                )
            );
        }
        return context_reader::Result<std::unique_ptr<context_reader::PdfDocument>>::success(
            std::make_unique<FakePdfDocument>()
        );
    }
};

}  // namespace

int main() {
    using namespace context_reader;

    constexpr PageSize page_size{600.0, 800.0};
    constexpr PagePoint page_point{125.25, 300.75};
    for(const auto rotation : {
            PageRotation::degrees_0,
            PageRotation::degrees_90,
            PageRotation::degrees_180,
            PageRotation::degrees_270,
        }) {
        auto transform_result = PageTransform::create(
            page_size,
            Scale{1.5},
            DevicePixelRatio{2.0},
            rotation
        );
        check(transform_result.has_value(), "valid transform is created");
        if(transform_result) {
            const auto transform = std::move(transform_result).value();
            const auto roundtrip = transform.to_page(transform.to_device(page_point));
            check(close(roundtrip.x, page_point.x), "x coordinate survives roundtrip");
            check(close(roundtrip.y, page_point.y), "y coordinate survives roundtrip");
        }
    }

    auto rotated = PageTransform::create(
        page_size,
        Scale{1.0},
        DevicePixelRatio{2.0},
        PageRotation::degrees_90
    );
    check(rotated.has_value(), "rotated transform is created");
    if(rotated) {
        const auto size = rotated.value().device_size();
        check(close(size.width, 1600.0), "quarter turn swaps device width");
        check(close(size.height, 1200.0), "quarter turn swaps device height");
    }

    const auto invalid = PageTransform::create(
        PageSize{0.0, 800.0},
        Scale{1.0},
        DevicePixelRatio{1.0},
        PageRotation::degrees_0
    );
    check(!invalid.has_value(), "zero page dimension is rejected");
    check(invalid.error().code() == ErrorCode::invalid_argument, "invalid geometry is typed");

    const auto invalid_rotation = PageTransform::create(
        page_size,
        Scale{1.0},
        DevicePixelRatio{1.0},
        static_cast<PageRotation>(45)
    );
    check(!invalid_rotation.has_value(), "unsupported page rotation is rejected");

    FakePdfEngine engine;
    const auto failed_session = DocumentSession::open(engine, std::filesystem::path{});
    check(!failed_session.has_value(), "document open errors cross the session boundary");

    auto session_result = DocumentSession::open(engine, std::filesystem::path("fixture.pdf"));
    check(session_result.has_value(), "document session opens through engine port");
    if(session_result) {
        auto session = std::move(session_result).value();
        check(session->page_count() == 2U, "session owns the opened document");
        const auto page = session->page_info(1U);
        check(page.has_value() && page.value().index == 1U, "session serializes page query");
        const auto missing = session->page_info(2U);
        check(!missing.has_value(), "session preserves adapter errors");
    }

    if(failures == 0) {
        std::cout << "pdf_foundation_test passed\n";
    }
    return failures == 0 ? 0 : 1;
}
