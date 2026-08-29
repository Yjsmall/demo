#include "context_reader/pdf/mupdf_engine.hpp"

#include <cstddef>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include "context_reader/pdf/page_geometry.hpp"
#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

namespace {

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] int normalized_rotation(int rotation) noexcept {
    if(rotation < 0) {
        rotation = 360 - ((-rotation) % 360);
    }
    rotation %= 360;
    rotation = 90 * ((rotation + 45) / 90);
    return rotation >= 360 ? 0 : rotation;
}

[[nodiscard]] PageRotation page_rotation(int rotation) noexcept {
    switch(rotation) {
        case 90:
            return PageRotation::degrees_90;
        case 180:
            return PageRotation::degrees_180;
        case 270:
            return PageRotation::degrees_270;
        default:
            return PageRotation::degrees_0;
    }
}

void append_utf8(std::string& output, int codepoint) {
    std::uint32_t value = 0xFFFDU;
    if(codepoint >= 0 && codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        value = static_cast<std::uint32_t>(codepoint);
    }

    if(value <= 0x7FU) {
        output.push_back(static_cast<char>(value));
    } else if(value <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if(value <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
}

[[nodiscard]] PageRect normalized_page_rect(const fz_rect& bounds, const PageTransform& transform) {
    const std::array<DevicePoint, 4> corners{
        DevicePoint{bounds.x0, bounds.y0},
        DevicePoint{bounds.x1, bounds.y0},
        DevicePoint{bounds.x1, bounds.y1},
        DevicePoint{bounds.x0, bounds.y1},
    };
    auto minimum_x = std::numeric_limits<double>::infinity();
    auto minimum_y = std::numeric_limits<double>::infinity();
    auto maximum_x = -std::numeric_limits<double>::infinity();
    auto maximum_y = -std::numeric_limits<double>::infinity();
    for(const auto corner : corners) {
        const auto page_point = transform.to_page(corner);
        minimum_x = std::min(minimum_x, page_point.x);
        minimum_y = std::min(minimum_y, page_point.y);
        maximum_x = std::max(maximum_x, page_point.x);
        maximum_y = std::max(maximum_y, page_point.y);
    }
    return PageRect{minimum_x, minimum_y, maximum_x - minimum_x, maximum_y - minimum_y};
}

void collect_text_lines(
    fz_stext_block* first_block,
    const PageTransform& transform,
    std::vector<TextLineInfo>& lines
) {
    for(auto* block = first_block; block != nullptr; block = block->next) {
        if(block->type == FZ_STEXT_BLOCK_TEXT) {
            for(auto* line = block->u.t.first_line; line != nullptr; line = line->next) {
                std::string text;
                for(auto* character = line->first_char; character != nullptr; character = character->next) {
                    append_utf8(text, character->c);
                }
                lines.push_back(TextLineInfo{
                    .text = std::move(text),
                    .bounds = normalized_page_rect(line->bbox, transform),
                    .vertical = line->wmode != 0,
                });
            }
        } else if(block->type == FZ_STEXT_BLOCK_STRUCT && block->u.s.down != nullptr) {
            collect_text_lines(block->u.s.down->first_block, transform, lines);
        }
    }
}

class MuPdfDocument final : public PdfDocument {
public:
    MuPdfDocument(fz_context* context, pdf_document* document, std::size_t page_count) noexcept
        : context_(context), document_(document), page_count_(page_count) {}

    ~MuPdfDocument() override {
        pdf_drop_document(context_, document_);
        fz_drop_context(context_);
    }

    [[nodiscard]] std::size_t page_count() const noexcept override { return page_count_; }

    [[nodiscard]] Result<PageInfo> page_info(std::size_t page_index) override {
        if(page_index >= page_count_) {
            return Result<PageInfo>::failure(
                Error(ErrorCode::not_found, "page index is out of range")
            );
        }

        fz_rect crop_box{};
        fz_matrix transform{};
        float user_unit = 1.0F;
        int rotation = 0;
        const char* caught_message = nullptr;

        fz_var(crop_box);
        fz_var(user_unit);
        fz_var(rotation);
        fz_var(caught_message);
        fz_try(context_) {
            pdf_obj* page_object = pdf_lookup_page_obj(
                context_,
                document_,
                static_cast<int>(page_index)
            );
            pdf_page_obj_transform_box(
                context_,
                page_object,
                &crop_box,
                &transform,
                FZ_CROP_BOX
            );
            user_unit = pdf_dict_get_real_default(
                context_,
                page_object,
                PDF_NAME(UserUnit),
                1.0F
            );
            rotation = normalized_rotation(
                pdf_dict_get_inheritable_int(context_, page_object, PDF_NAME(Rotate))
            );
        }
        fz_catch(context_) {
            caught_message = fz_caught_message(context_);
        }

        if(caught_message != nullptr) {
            return Result<PageInfo>::failure(
                Error(ErrorCode::pdf_failure, std::string(caught_message))
            );
        }

        const auto width = static_cast<double>(crop_box.x1 - crop_box.x0) * user_unit;
        const auto height = static_cast<double>(crop_box.y1 - crop_box.y0) * user_unit;
        return Result<PageInfo>::success(PageInfo{
            .index = page_index,
            .size = PageSize{width, height},
            .rotation = page_rotation(rotation),
        });
    }

    [[nodiscard]] Result<EncodedPageImage> render_page_png(
        std::size_t page_index,
        double pixels_per_point
    ) override {
        if(page_index >= page_count_) {
            return Result<EncodedPageImage>::failure(
                Error(ErrorCode::not_found, "page index is out of range")
            );
        }
        if(!std::isfinite(pixels_per_point) || pixels_per_point <= 0.0 || pixels_per_point > 16.0) {
            return Result<EncodedPageImage>::failure(
                Error(ErrorCode::invalid_argument, "pixels per point must be in (0, 16]")
            );
        }

        pdf_page* page = nullptr;
        fz_pixmap* pixmap = nullptr;
        fz_buffer* png_buffer = nullptr;
        unsigned char* png_data = nullptr;
        std::size_t png_size = 0;
        int width = 0;
        int height = 0;
        const char* caught_message = nullptr;
        fz_var(page);
        fz_var(pixmap);
        fz_var(png_buffer);
        fz_var(png_data);
        fz_var(png_size);
        fz_var(width);
        fz_var(height);
        fz_var(caught_message);

        fz_try(context_) {
            page = pdf_load_page(context_, document_, static_cast<int>(page_index));
            pixmap = pdf_new_pixmap_from_page_with_usage(
                context_,
                page,
                fz_scale(static_cast<float>(pixels_per_point), static_cast<float>(pixels_per_point)),
                fz_device_rgb(context_),
                0,
                "View",
                FZ_CROP_BOX
            );
            width = fz_pixmap_width(context_, pixmap);
            height = fz_pixmap_height(context_, pixmap);
            png_buffer = fz_new_buffer_from_pixmap_as_png(
                context_,
                pixmap,
                fz_default_color_params
            );
            png_size = fz_buffer_storage(context_, png_buffer, &png_data);
        }
        fz_catch(context_) {
            caught_message = fz_caught_message(context_);
        }

        if(caught_message != nullptr) {
            const std::string message(caught_message);
            fz_drop_buffer(context_, png_buffer);
            fz_drop_pixmap(context_, pixmap);
            pdf_drop_page(context_, page);
            return Result<EncodedPageImage>::failure(Error(ErrorCode::pdf_failure, message));
        }
        if(width <= 0 || height <= 0 || png_data == nullptr || png_size == 0) {
            fz_drop_buffer(context_, png_buffer);
            fz_drop_pixmap(context_, pixmap);
            pdf_drop_page(context_, page);
            return Result<EncodedPageImage>::failure(
                Error(ErrorCode::pdf_failure, "MuPDF returned an invalid page image")
            );
        }

        std::vector<std::uint8_t> png(png_data, png_data + png_size);
        fz_drop_buffer(context_, png_buffer);
        fz_drop_pixmap(context_, pixmap);
        pdf_drop_page(context_, page);
        return Result<EncodedPageImage>::success(EncodedPageImage{
            .width_pixels = static_cast<std::size_t>(width),
            .height_pixels = static_cast<std::size_t>(height),
            .pixels_per_point = pixels_per_point,
            .png = std::move(png),
        });
    }

    [[nodiscard]] Result<PageText> extract_text(std::size_t page_index) override {
        const auto info_result = page_info(page_index);
        if(!info_result) {
            return Result<PageText>::failure(info_result.error());
        }
        auto transform_result = PageTransform::create(
            info_result.value().size,
            Scale{1.0},
            DevicePixelRatio{1.0},
            info_result.value().rotation
        );
        if(!transform_result) {
            return Result<PageText>::failure(transform_result.error());
        }

        pdf_page* page = nullptr;
        fz_stext_page* text_page = nullptr;
        fz_buffer* text_buffer = nullptr;
        unsigned char* text_data = nullptr;
        std::size_t text_size = 0;
        const char* caught_message = nullptr;
        fz_stext_options options{};
        fz_var(page);
        fz_var(text_page);
        fz_var(text_buffer);
        fz_var(text_data);
        fz_var(text_size);
        fz_var(caught_message);

        fz_try(context_) {
            page = pdf_load_page(context_, document_, static_cast<int>(page_index));
            text_page = fz_new_stext_page_from_page(
                context_,
                reinterpret_cast<fz_page*>(page),
                &options
            );
            text_buffer = fz_new_buffer_from_stext_page(context_, text_page);
            text_size = fz_buffer_storage(context_, text_buffer, &text_data);
        }
        fz_catch(context_) {
            caught_message = fz_caught_message(context_);
        }

        if(caught_message != nullptr) {
            const std::string message(caught_message);
            fz_drop_buffer(context_, text_buffer);
            fz_drop_stext_page(context_, text_page);
            pdf_drop_page(context_, page);
            return Result<PageText>::failure(Error(ErrorCode::pdf_failure, message));
        }

        PageText result;
        if(text_data != nullptr && text_size > 0) {
            result.text.assign(reinterpret_cast<const char*>(text_data), text_size);
        }
        collect_text_lines(
            text_page->first_block,
            transform_result.value(),
            result.lines
        );
        fz_drop_buffer(context_, text_buffer);
        fz_drop_stext_page(context_, text_page);
        pdf_drop_page(context_, page);
        return Result<PageText>::success(std::move(result));
    }

private:
    fz_context* context_;
    pdf_document* document_;
    std::size_t page_count_;
};

}  // namespace

Result<std::unique_ptr<PdfDocument>> MuPdfEngine::open(
    const std::filesystem::path& source
) {
    if(source.empty()) {
        return Result<std::unique_ptr<PdfDocument>>::failure(
            Error(ErrorCode::invalid_argument, "PDF source path is empty")
        );
    }

    std::error_code file_error;
    if(!std::filesystem::is_regular_file(source, file_error)) {
        return Result<std::unique_ptr<PdfDocument>>::failure(
            Error(ErrorCode::not_found, "PDF source file was not found")
        );
    }

    fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if(context == nullptr) {
        return Result<std::unique_ptr<PdfDocument>>::failure(
            Error(ErrorCode::resource_exhausted, "MuPDF context creation failed")
        );
    }

    pdf_document* document = nullptr;
    const char* caught_message = nullptr;
    int page_count = 0;
    int needs_password = 0;
    const auto source_utf8 = utf8_path(source);

    fz_var(document);
    fz_var(caught_message);
    fz_var(page_count);
    fz_var(needs_password);
    fz_try(context) {
        document = pdf_open_document(context, source_utf8.c_str());
        needs_password = pdf_needs_password(context, document);
        if(!needs_password) {
            page_count = pdf_count_pages(context, document);
        }
    }
    fz_catch(context) {
        caught_message = fz_caught_message(context);
    }

    if(caught_message != nullptr) {
        const std::string message(caught_message);
        pdf_drop_document(context, document);
        fz_drop_context(context);
        return Result<std::unique_ptr<PdfDocument>>::failure(
            Error(ErrorCode::pdf_failure, message)
        );
    }
    if(needs_password != 0) {
        pdf_drop_document(context, document);
        fz_drop_context(context);
        return Result<std::unique_ptr<PdfDocument>>::failure(
            Error(ErrorCode::password_required, "PDF password is required")
        );
    }
    if(page_count < 0) {
        pdf_drop_document(context, document);
        fz_drop_context(context);
        return Result<std::unique_ptr<PdfDocument>>::failure(
            Error(ErrorCode::pdf_failure, "MuPDF returned an invalid page count")
        );
    }

    return Result<std::unique_ptr<PdfDocument>>::success(
        std::make_unique<MuPdfDocument>(
            context,
            document,
            static_cast<std::size_t>(page_count)
        )
    );
}

}  // namespace context_reader
