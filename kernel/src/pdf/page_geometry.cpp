#include "context_reader/pdf/page_geometry.hpp"

#include <cmath>

#include "context_reader/shared/error.hpp"

namespace context_reader {

namespace {

[[nodiscard]] bool is_positive_finite(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool is_supported_rotation(PageRotation rotation) noexcept {
    switch(rotation) {
        case PageRotation::degrees_0:
        case PageRotation::degrees_90:
        case PageRotation::degrees_180:
        case PageRotation::degrees_270:
            return true;
    }
    return false;
}

}  // namespace

Result<PageTransform> PageTransform::create(
    PageSize page_size,
    Scale scale,
    DevicePixelRatio device_pixel_ratio,
    PageRotation rotation
) {
    if(!is_positive_finite(page_size.width) || !is_positive_finite(page_size.height)) {
        return Result<PageTransform>::failure(
            Error(ErrorCode::invalid_argument, "page dimensions must be positive and finite")
        );
    }
    if(!is_positive_finite(scale.value) || !is_positive_finite(device_pixel_ratio.value)) {
        return Result<PageTransform>::failure(
            Error(ErrorCode::invalid_argument, "scale and DPR must be positive and finite")
        );
    }
    const auto pixels_per_point = scale.value * device_pixel_ratio.value;
    if(!is_positive_finite(pixels_per_point)) {
        return Result<PageTransform>::failure(
            Error(ErrorCode::invalid_argument, "combined scale and DPR must be finite")
        );
    }
    if(!is_supported_rotation(rotation)) {
        return Result<PageTransform>::failure(
            Error(ErrorCode::invalid_argument, "page rotation is not supported")
        );
    }

    return Result<PageTransform>::success(
        PageTransform(page_size, pixels_per_point, rotation)
    );
}

PageTransform::PageTransform(
    PageSize page_size,
    double pixels_per_point,
    PageRotation rotation
) noexcept
    : page_size_(page_size), pixels_per_point_(pixels_per_point), rotation_(rotation) {}

DevicePoint PageTransform::to_device(PagePoint point) const noexcept {
    PagePoint rotated{};
    switch(rotation_) {
        case PageRotation::degrees_0:
            rotated = point;
            break;
        case PageRotation::degrees_90:
            rotated = PagePoint{page_size_.height - point.y, point.x};
            break;
        case PageRotation::degrees_180:
            rotated = PagePoint{page_size_.width - point.x, page_size_.height - point.y};
            break;
        case PageRotation::degrees_270:
            rotated = PagePoint{point.y, page_size_.width - point.x};
            break;
    }
    return DevicePoint{
        rotated.x * pixels_per_point_,
        rotated.y * pixels_per_point_,
    };
}

PagePoint PageTransform::to_page(DevicePoint point) const noexcept {
    const PagePoint rotated{
        point.x / pixels_per_point_,
        point.y / pixels_per_point_,
    };
    switch(rotation_) {
        case PageRotation::degrees_0:
            return rotated;
        case PageRotation::degrees_90:
            return PagePoint{rotated.y, page_size_.height - rotated.x};
        case PageRotation::degrees_180:
            return PagePoint{page_size_.width - rotated.x, page_size_.height - rotated.y};
        case PageRotation::degrees_270:
            return PagePoint{page_size_.width - rotated.y, rotated.x};
    }
    return rotated;
}

PageSize PageTransform::device_size() const noexcept {
    const bool swaps_axes = rotation_ == PageRotation::degrees_90 ||
                            rotation_ == PageRotation::degrees_270;
    const auto width = swaps_axes ? page_size_.height : page_size_.width;
    const auto height = swaps_axes ? page_size_.width : page_size_.height;
    return PageSize{width * pixels_per_point_, height * pixels_per_point_};
}

}  // namespace context_reader
