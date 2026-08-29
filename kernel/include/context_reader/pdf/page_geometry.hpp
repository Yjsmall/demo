#pragma once

#include <cmath>
#include <cstdint>

#include "context_reader/shared/result.hpp"

namespace context_reader {

struct PagePoint final {
    double x;
    double y;

    bool operator==(const PagePoint&) const = default;
};

struct DevicePoint final {
    double x;
    double y;

    bool operator==(const DevicePoint&) const = default;
};

struct PageSize final {
    double width;
    double height;

    bool operator==(const PageSize&) const = default;
};

struct PageRect final {
    double x;
    double y;
    double width;
    double height;

    bool operator==(const PageRect&) const = default;
};

struct Scale final {
    double value;
};

struct DevicePixelRatio final {
    double value;
};

enum class PageRotation : std::uint16_t {
    degrees_0 = 0,
    degrees_90 = 90,
    degrees_180 = 180,
    degrees_270 = 270,
};

// Page coordinates use the CropBox top-left as origin, with both axes in points.
// Device coordinates use the rendered page top-left as origin, in physical pixels.
class PageTransform final {
public:
    [[nodiscard]] static Result<PageTransform> create(
        PageSize page_size,
        Scale scale,
        DevicePixelRatio device_pixel_ratio,
        PageRotation rotation
    );

    [[nodiscard]] PagePoint to_page(DevicePoint point) const noexcept;
    [[nodiscard]] DevicePoint to_device(PagePoint point) const noexcept;
    [[nodiscard]] PageSize device_size() const noexcept;

private:
    PageTransform(
        PageSize page_size,
        double pixels_per_point,
        PageRotation rotation
    ) noexcept;

    PageSize page_size_;
    double pixels_per_point_;
    PageRotation rotation_;
};

}  // namespace context_reader
