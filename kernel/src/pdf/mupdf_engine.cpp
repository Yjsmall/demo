#include "context_reader/pdf/mupdf_engine.hpp"

#include <cstddef>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
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

struct MuPdfLockState final {
    MuPdfLockState() noexcept {
        callbacks.user = this;
        callbacks.lock = [](void* user, int lock) noexcept {
            static_cast<MuPdfLockState*>(user)->mutexes[static_cast<std::size_t>(lock)].lock();
        };
        callbacks.unlock = [](void* user, int lock) noexcept {
            static_cast<MuPdfLockState*>(user)->mutexes[static_cast<std::size_t>(lock)].unlock();
        };
    }

    std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    fz_locks_context callbacks{};
};

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

[[nodiscard]] PageQuad normalized_page_quad(const fz_quad& quad, const PageTransform& transform) {
    return PageQuad{
        .upper_left = transform.to_page(DevicePoint{quad.ul.x, quad.ul.y}),
        .upper_right = transform.to_page(DevicePoint{quad.ur.x, quad.ur.y}),
        .lower_left = transform.to_page(DevicePoint{quad.ll.x, quad.ll.y}),
        .lower_right = transform.to_page(DevicePoint{quad.lr.x, quad.lr.y}),
    };
}

[[nodiscard]] TextDirection text_direction(const fz_stext_line& line, const fz_stext_char& character) noexcept {
    if(line.wmode != 0) {
        return TextDirection::top_to_bottom;
    }
    return character.bidi != 0 ? TextDirection::right_to_left : TextDirection::left_to_right;
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

void collect_text_layout(
    fz_stext_block* first_block,
    const PageTransform& transform,
    PageTextLayout& layout,
    std::size_t& logical_offset,
    std::size_t& line_index
) {
    for(auto* block = first_block; block != nullptr; block = block->next) {
        if(block->type == FZ_STEXT_BLOCK_TEXT) {
            for(auto* line = block->u.t.first_line; line != nullptr; line = line->next) {
                std::string line_text;
                for(auto* character = line->first_char; character != nullptr; character = character->next) {
                    std::string value;
                    append_utf8(value, character->c);
                    line_text.append(value);
                    layout.text.append(value);
                    layout.units.push_back(TextSelectionUnit{
                        .logical_start = logical_offset,
                        .logical_end = logical_offset + 1,
                        .text = std::move(value),
                        .direction = text_direction(*line, *character),
                        .line_index = line_index,
                        .quad = normalized_page_quad(character->quad, transform),
                    });
                    ++logical_offset;
                }
                layout.lines.push_back(TextLineInfo{
                    .text = std::move(line_text),
                    .bounds = normalized_page_rect(line->bbox, transform),
                    .vertical = line->wmode != 0,
                });
                if(line->next != nullptr) {
                    layout.text.push_back('\n');
                    ++logical_offset;
                }
                ++line_index;
            }
        } else if(block->type == FZ_STEXT_BLOCK_STRUCT && block->u.s.down != nullptr) {
            collect_text_layout(
                block->u.s.down->first_block,
                transform,
                layout,
                logical_offset,
                line_index
            );
        }
    }
}

[[nodiscard]] double squared_distance(PagePoint point, const PageQuad& quad) noexcept {
    const auto center_x = (quad.upper_left.x + quad.upper_right.x + quad.lower_left.x + quad.lower_right.x) / 4.0;
    const auto center_y = (quad.upper_left.y + quad.upper_right.y + quad.lower_left.y + quad.lower_right.y) / 4.0;
    const auto dx = point.x - center_x;
    const auto dy = point.y - center_y;
    return dx * dx + dy * dy;
}

[[nodiscard]] std::string unit_range_text(
    const std::vector<TextSelectionUnit>& units,
    std::size_t begin,
    std::size_t end
) {
    std::string text;
    std::size_t previous_line = units[begin].line_index;
    for(std::size_t index = begin; index < end; ++index) {
        if(index != begin && units[index].line_index != previous_line) {
            text.push_back('\n');
        }
        text.append(units[index].text);
        previous_line = units[index].line_index;
    }
    return text;
}

class MuPdfTileDisplayList final : public PdfTileDisplayList {
public:
    MuPdfTileDisplayList(
        fz_context* context,
        fz_display_list* display_list,
        fz_rect bounds,
        PageInfo page_info,
        std::size_t estimated_bytes,
        std::shared_ptr<MuPdfLockState> locks
    ) noexcept
        : context_(context),
          display_list_(display_list),
          bounds_(bounds),
          page_info_(page_info),
          estimated_bytes_(estimated_bytes),
          locks_(std::move(locks)) {}

    ~MuPdfTileDisplayList() override {
        fz_drop_display_list(context_, display_list_);
        fz_drop_context(context_);
    }

    [[nodiscard]] std::size_t estimated_bytes() const noexcept override {
        return estimated_bytes_;
    }

    [[nodiscard]] Result<RenderedTile> render(const TileRequest& request) override {
        if(request.page_index != page_info_.index || !std::isfinite(request.pixels_per_point) ||
           request.pixels_per_point <= 0.0 || request.pixels_per_point > 16.0 ||
           request.width_pixels == 0 || request.height_pixels == 0 ||
           request.width_pixels > 512 || request.height_pixels > 512) {
            return Result<RenderedTile>::failure(
                Error(ErrorCode::invalid_argument, "Tile dimensions or scale are outside the supported range")
            );
        }
        const auto transform = PageTransform::create(
            page_info_.size,
            Scale{request.pixels_per_point},
            DevicePixelRatio{1.0},
            page_info_.rotation
        );
        if(!transform) return Result<RenderedTile>::failure(transform.error());
        const auto size = transform.value().device_size();
        const auto page_width = static_cast<std::size_t>(std::ceil(size.width));
        const auto page_height = static_cast<std::size_t>(std::ceil(size.height));
        if(page_width > 16384 || page_height > 16384 ||
           page_width > 134217728ULL / std::max<std::size_t>(page_height, 1) ||
           request.x_pixels > page_width || request.y_pixels > page_height ||
           request.width_pixels > page_width - request.x_pixels ||
           request.height_pixels > page_height - request.y_pixels) {
            return Result<RenderedTile>::failure(
                Error(ErrorCode::invalid_argument, "Tile is outside the bounded page raster")
            );
        }

        fz_context* worker = fz_clone_context(context_);
        if(worker == nullptr) {
            return Result<RenderedTile>::failure(
                Error(ErrorCode::resource_exhausted, "MuPDF worker context creation failed")
            );
        }
        fz_pixmap* pixmap = nullptr;
        fz_device* device = nullptr;
        const char* caught_message = nullptr;
        fz_var(pixmap);
        fz_var(device);
        fz_var(caught_message);
        const auto matrix = fz_scale(
            static_cast<float>(request.pixels_per_point),
            static_cast<float>(request.pixels_per_point)
        );
        const auto page_bounds = fz_round_rect(fz_transform_rect(bounds_, matrix));
        const auto tile_bounds = fz_make_irect(
            page_bounds.x0 + static_cast<int>(request.x_pixels),
            page_bounds.y0 + static_cast<int>(request.y_pixels),
            page_bounds.x0 + static_cast<int>(request.x_pixels + request.width_pixels),
            page_bounds.y0 + static_cast<int>(request.y_pixels + request.height_pixels)
        );
        fz_try(worker) {
            pixmap = fz_new_pixmap_with_bbox(worker, fz_device_rgb(worker), tile_bounds, nullptr, 1);
            fz_clear_pixmap_with_value(worker, pixmap, 255);
            device = fz_new_draw_device_with_bbox(worker, fz_identity, pixmap, &tile_bounds);
            fz_run_display_list(worker, display_list_, device, matrix, fz_rect_from_irect(tile_bounds), nullptr);
            fz_close_device(worker, device);
        }
        fz_catch(worker) {
            caught_message = fz_caught_message(worker);
        }
        if(caught_message != nullptr) {
            const std::string message(caught_message);
            fz_drop_device(worker, device);
            fz_drop_pixmap(worker, pixmap);
            fz_drop_context(worker);
            return Result<RenderedTile>::failure(Error(ErrorCode::pdf_failure, message));
        }

        const auto* samples = fz_pixmap_samples(worker, pixmap);
        const auto stride = fz_pixmap_stride(worker, pixmap);
        const auto components = fz_pixmap_components(worker, pixmap);
        if(samples == nullptr || stride <= 0 || components != 4) {
            fz_drop_device(worker, device);
            fz_drop_pixmap(worker, pixmap);
            fz_drop_context(worker);
            return Result<RenderedTile>::failure(
                Error(ErrorCode::pdf_failure, "MuPDF returned an invalid RGBA tile")
            );
        }
        std::vector<std::uint8_t> rgba(request.width_pixels * request.height_pixels * 4);
        for(std::size_t row = 0; row < request.height_pixels; ++row) {
            std::copy_n(
                samples + row * static_cast<std::size_t>(stride),
                request.width_pixels * 4,
                rgba.data() + row * request.width_pixels * 4
            );
        }
        fz_drop_device(worker, device);
        fz_drop_pixmap(worker, pixmap);
        fz_drop_context(worker);
        return Result<RenderedTile>::success(RenderedTile{
            .page_index = request.page_index,
            .x_pixels = request.x_pixels,
            .y_pixels = request.y_pixels,
            .width_pixels = request.width_pixels,
            .height_pixels = request.height_pixels,
            .pixels_per_point = request.pixels_per_point,
            .generation = request.generation,
            .rgba = std::move(rgba),
        });
    }

private:
    fz_context* context_;
    fz_display_list* display_list_;
    fz_rect bounds_;
    PageInfo page_info_;
    std::size_t estimated_bytes_;
    std::shared_ptr<MuPdfLockState> locks_;
};

class MuPdfDocument final : public PdfDocument {
public:
    MuPdfDocument(
        fz_context* context,
        pdf_document* document,
        std::size_t page_count,
        std::shared_ptr<MuPdfLockState> locks
    ) noexcept
        : context_(context), document_(document), page_count_(page_count), locks_(std::move(locks)) {}

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
        const auto info_result = page_info(page_index);
        if(!info_result) {
            return Result<EncodedPageImage>::failure(info_result.error());
        }
        const auto transform_result = PageTransform::create(
            info_result.value().size,
            Scale{pixels_per_point},
            DevicePixelRatio{1.0},
            info_result.value().rotation
        );
        if(!transform_result) {
            return Result<EncodedPageImage>::failure(transform_result.error());
        }
        const auto device_size = transform_result.value().device_size();
        if(device_size.width > 16384.0 || device_size.height > 16384.0 ||
           device_size.width * device_size.height > 134217728.0) {
            return Result<EncodedPageImage>::failure(
                Error(ErrorCode::invalid_argument, "Rendered page exceeds the raster limits")
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

    [[nodiscard]] Result<RenderedTile> render_tile(const TileRequest& request) override {
        if(request.page_index >= page_count_) {
            return Result<RenderedTile>::failure(Error(ErrorCode::not_found, "page index is out of range"));
        }
        if(!std::isfinite(request.pixels_per_point) || request.pixels_per_point <= 0.0 ||
           request.pixels_per_point > 16.0 || request.width_pixels == 0 || request.height_pixels == 0 ||
           request.width_pixels > 512 || request.height_pixels > 512) {
            return Result<RenderedTile>::failure(
                Error(ErrorCode::invalid_argument, "Tile dimensions or scale are outside the supported range")
            );
        }
        const auto info_result = page_info(request.page_index);
        if(!info_result) {
            return Result<RenderedTile>::failure(info_result.error());
        }
        const auto transform_result = PageTransform::create(
            info_result.value().size,
            Scale{request.pixels_per_point},
            DevicePixelRatio{1.0},
            info_result.value().rotation
        );
        if(!transform_result) {
            return Result<RenderedTile>::failure(transform_result.error());
        }
        const auto size = transform_result.value().device_size();
        const auto page_width = static_cast<std::size_t>(std::ceil(size.width));
        const auto page_height = static_cast<std::size_t>(std::ceil(size.height));
        if(page_width > 16384 || page_height > 16384 ||
           page_width > 134217728ULL / std::max<std::size_t>(page_height, 1) ||
           request.x_pixels > page_width || request.y_pixels > page_height ||
           request.width_pixels > page_width - request.x_pixels ||
           request.height_pixels > page_height - request.y_pixels) {
            return Result<RenderedTile>::failure(
                Error(ErrorCode::invalid_argument, "Tile is outside the bounded page raster")
            );
        }

        pdf_page* page = nullptr;
        fz_pixmap* pixmap = nullptr;
        fz_device* device = nullptr;
        const char* caught_message = nullptr;
        fz_var(page);
        fz_var(pixmap);
        fz_var(device);
        fz_var(caught_message);
        fz_try(context_) {
            page = pdf_load_page(context_, document_, static_cast<int>(request.page_index));
            const auto matrix = fz_scale(
                static_cast<float>(request.pixels_per_point),
                static_cast<float>(request.pixels_per_point)
            );
            const auto page_bounds = fz_round_rect(fz_transform_rect(fz_bound_page(context_, reinterpret_cast<fz_page*>(page)), matrix));
            const auto tile_bounds = fz_make_irect(
                page_bounds.x0 + static_cast<int>(request.x_pixels),
                page_bounds.y0 + static_cast<int>(request.y_pixels),
                page_bounds.x0 + static_cast<int>(request.x_pixels + request.width_pixels),
                page_bounds.y0 + static_cast<int>(request.y_pixels + request.height_pixels)
            );
            pixmap = fz_new_pixmap_with_bbox(context_, fz_device_rgb(context_), tile_bounds, nullptr, 1);
            fz_clear_pixmap_with_value(context_, pixmap, 255);
            device = fz_new_draw_device_with_bbox(context_, fz_identity, pixmap, &tile_bounds);
            fz_run_page(context_, reinterpret_cast<fz_page*>(page), device, matrix, nullptr);
            fz_close_device(context_, device);
        }
        fz_catch(context_) {
            caught_message = fz_caught_message(context_);
        }
        if(caught_message != nullptr) {
            const std::string message(caught_message);
            fz_drop_device(context_, device);
            fz_drop_pixmap(context_, pixmap);
            pdf_drop_page(context_, page);
            return Result<RenderedTile>::failure(Error(ErrorCode::pdf_failure, message));
        }

        const auto* samples = fz_pixmap_samples(context_, pixmap);
        const auto stride = fz_pixmap_stride(context_, pixmap);
        const auto components = fz_pixmap_components(context_, pixmap);
        if(samples == nullptr || stride <= 0 || components != 4) {
            fz_drop_device(context_, device);
            fz_drop_pixmap(context_, pixmap);
            pdf_drop_page(context_, page);
            return Result<RenderedTile>::failure(Error(ErrorCode::pdf_failure, "MuPDF returned an invalid RGBA tile"));
        }
        std::vector<std::uint8_t> rgba(request.width_pixels * request.height_pixels * 4);
        for(std::size_t row = 0; row < request.height_pixels; ++row) {
            std::copy_n(
                samples + row * static_cast<std::size_t>(stride),
                request.width_pixels * 4,
                rgba.data() + row * request.width_pixels * 4
            );
        }
        fz_drop_device(context_, device);
        fz_drop_pixmap(context_, pixmap);
        pdf_drop_page(context_, page);
        return Result<RenderedTile>::success(RenderedTile{
            .page_index = request.page_index,
            .x_pixels = request.x_pixels,
            .y_pixels = request.y_pixels,
            .width_pixels = request.width_pixels,
            .height_pixels = request.height_pixels,
            .pixels_per_point = request.pixels_per_point,
            .generation = request.generation,
            .rgba = std::move(rgba),
        });
    }

    [[nodiscard]] Result<std::shared_ptr<PdfTileDisplayList>> create_tile_display_list(
        std::size_t page_index
    ) override {
        const auto info = page_info(page_index);
        if(!info) {
            return Result<std::shared_ptr<PdfTileDisplayList>>::failure(info.error());
        }
        const auto unit_transform = PageTransform::create(
            info.value().size,
            Scale{1.0},
            DevicePixelRatio{1.0},
            info.value().rotation
        );
        if(!unit_transform) {
            return Result<std::shared_ptr<PdfTileDisplayList>>::failure(unit_transform.error());
        }
        const auto unit_size = unit_transform.value().device_size();
        if(unit_size.width > 16384.0 || unit_size.height > 16384.0 ||
           unit_size.width * unit_size.height > 134217728.0) {
            return Result<std::shared_ptr<PdfTileDisplayList>>::failure(
                Error(ErrorCode::invalid_argument, "Page exceeds the bounded tile raster")
            );
        }
        pdf_page* page = nullptr;
        fz_display_list* display_list = nullptr;
        fz_device* device = nullptr;
        fz_context* owner_context = nullptr;
        fz_rect bounds{};
        const char* caught_message = nullptr;
        fz_var(page);
        fz_var(display_list);
        fz_var(device);
        fz_var(owner_context);
        fz_var(bounds);
        fz_var(caught_message);
        fz_try(context_) {
            page = pdf_load_page(context_, document_, static_cast<int>(page_index));
            bounds = fz_bound_page(context_, reinterpret_cast<fz_page*>(page));
            display_list = fz_new_display_list(context_, bounds);
            device = fz_new_list_device(context_, display_list);
            fz_run_page(context_, reinterpret_cast<fz_page*>(page), device, fz_identity, nullptr);
            fz_close_device(context_, device);
            owner_context = fz_clone_context(context_);
        }
        fz_catch(context_) {
            caught_message = fz_caught_message(context_);
        }
        fz_drop_device(context_, device);
        pdf_drop_page(context_, page);
        if(caught_message != nullptr || owner_context == nullptr) {
            const std::string message = caught_message == nullptr
                ? "MuPDF display-list context creation failed"
                : std::string(caught_message);
            fz_drop_display_list(context_, display_list);
            fz_drop_context(owner_context);
            return Result<std::shared_ptr<PdfTileDisplayList>>::failure(
                Error(caught_message == nullptr ? ErrorCode::resource_exhausted : ErrorCode::pdf_failure, message)
            );
        }
        const auto width = std::max(0.0F, bounds.x1 - bounds.x0);
        const auto height = std::max(0.0F, bounds.y1 - bounds.y0);
        const auto estimate_value = std::ceil(static_cast<double>(width) * height * 4.0);
        const auto estimate = estimate_value >= static_cast<double>(128U * 1024U * 1024U)
            ? 128U * 1024U * 1024U
            : std::max<std::size_t>(static_cast<std::size_t>(estimate_value), 64U * 1024U);
        return Result<std::shared_ptr<PdfTileDisplayList>>::success(
            std::make_shared<MuPdfTileDisplayList>(
                owner_context,
                display_list,
                bounds,
                info.value(),
                estimate,
                locks_
            )
        );
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

    [[nodiscard]] Result<PageTextLayout> page_text_layout(std::size_t page_index) override {
        const auto info_result = page_info(page_index);
        if(!info_result) {
            return Result<PageTextLayout>::failure(info_result.error());
        }
        const auto transform_result = PageTransform::create(
            info_result.value().size,
            Scale{1.0},
            DevicePixelRatio{1.0},
            info_result.value().rotation
        );
        if(!transform_result) {
            return Result<PageTextLayout>::failure(transform_result.error());
        }
        pdf_page* page = nullptr;
        fz_stext_page* text_page = nullptr;
        const char* caught_message = nullptr;
        fz_stext_options options{};
        fz_var(page);
        fz_var(text_page);
        fz_var(caught_message);
        fz_try(context_) {
            page = pdf_load_page(context_, document_, static_cast<int>(page_index));
            text_page = fz_new_stext_page_from_page(context_, reinterpret_cast<fz_page*>(page), &options);
        }
        fz_catch(context_) {
            caught_message = fz_caught_message(context_);
        }
        if(caught_message != nullptr) {
            const std::string message(caught_message);
            fz_drop_stext_page(context_, text_page);
            pdf_drop_page(context_, page);
            return Result<PageTextLayout>::failure(Error(ErrorCode::pdf_failure, message));
        }
        PageTextLayout result{
            .page_index = page_index,
            .layout_version = 1,
        };
        std::size_t logical_offset = 0;
        std::size_t line_index = 0;
        collect_text_layout(text_page->first_block, transform_result.value(), result, logical_offset, line_index);
        fz_drop_stext_page(context_, text_page);
        pdf_drop_page(context_, page);
        return Result<PageTextLayout>::success(std::move(result));
    }

    [[nodiscard]] Result<TextSelection> select_text(
        std::size_t page_index,
        PagePoint start_point,
        PagePoint end_point
    ) override {
        if(!std::isfinite(start_point.x) || !std::isfinite(start_point.y) ||
           !std::isfinite(end_point.x) || !std::isfinite(end_point.y)) {
            return Result<TextSelection>::failure(Error(ErrorCode::invalid_argument, "Selection points must be finite"));
        }
        auto layout_result = page_text_layout(page_index);
        if(!layout_result) {
            return Result<TextSelection>::failure(layout_result.error());
        }
        auto layout = std::move(layout_result).value();
        if(layout.units.empty()) {
            return Result<TextSelection>::failure(Error(ErrorCode::not_found, "Page contains no selectable text"));
        }
        const auto nearest = [&layout](PagePoint point) {
            std::size_t selected = 0;
            auto distance = squared_distance(point, layout.units.front().quad);
            for(std::size_t index = 1; index < layout.units.size(); ++index) {
                const auto candidate = squared_distance(point, layout.units[index].quad);
                if(candidate < distance) {
                    selected = index;
                    distance = candidate;
                }
            }
            return selected;
        };
        const auto first = nearest(start_point);
        const auto last = nearest(end_point);
        const auto begin = std::min(first, last);
        const auto end = std::max(first, last) + 1;
        TextSelection result{
            .page_index = page_index,
            .layout_version = layout.layout_version,
            .logical_start = layout.units[begin].logical_start,
            .logical_end = layout.units[end - 1].logical_end,
            .direction = layout.units[begin].direction,
            .text = unit_range_text(layout.units, begin, end),
        };
        result.quads.reserve(end - begin);
        for(std::size_t index = begin; index < end; ++index) {
            result.quads.push_back(layout.units[index].quad);
        }
        const auto prefix_begin = begin > 32 ? begin - 32 : 0;
        if(prefix_begin < begin) {
            result.quote_prefix = unit_range_text(layout.units, prefix_begin, begin);
        }
        const auto suffix_end = std::min(layout.units.size(), end + 32);
        if(end < suffix_end) {
            result.quote_suffix = unit_range_text(layout.units, end, suffix_end);
        }
        return Result<TextSelection>::success(std::move(result));
    }

private:
    fz_context* context_;
    pdf_document* document_;
    std::size_t page_count_;
    std::shared_ptr<MuPdfLockState> locks_;
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

    std::shared_ptr<MuPdfLockState> locks;
    try {
        locks = std::make_shared<MuPdfLockState>();
    } catch(const std::bad_alloc&) {
        return Result<std::unique_ptr<PdfDocument>>::failure(
            Error(ErrorCode::resource_exhausted, "MuPDF lock allocation failed")
        );
    }
    fz_context* context = fz_new_context(nullptr, &locks->callbacks, FZ_STORE_DEFAULT);
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
            static_cast<std::size_t>(page_count),
            std::move(locks)
        )
    );
}

}  // namespace context_reader
