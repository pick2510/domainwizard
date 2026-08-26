#pragma once

#include "wrftools/crs.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace wrftools {

struct WarpedRaster {
    std::vector<float> values;  // NaN for nodata, row-major top-down
    int width{};
    int height{};
    Bounds2D bounds3857;
};

// Builds a georeferenced in-memory GDAL dataset from a plain array and warps
// it to EPSG:3857, returning the warped array and its bounds - this is what
// makes the resulting overlay axis-aligned in tile space, the reason to
// reproject here rather than draw in the WRF file's native CRS (which would
// need a per-vertex/pixel reprojection in the paint path). Mirrors
// wrftools.rasterlayer._warp_to_web_mercator.
[[nodiscard]] WarpedRaster warpToWebMercator(std::span<const float> values, int width, int height,
    const std::string& sourceWkt, const std::array<double, 6>& sourceGeotransform);

}  // namespace wrftools
