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

// A fully-specified destination raster grid: what warpToGrid's dest* /
// destGeotransform / destWidth / destHeight arguments describe, bundled so
// a caller can compute one ONCE (see suggestWarpGrid) and reuse it for
// every slice of a multi-variable, multi-level, multi-timestep run rather
// than re-deriving it per warp.
struct DestinationGrid {
    std::string wkt;
    std::array<double, 6> geotransform{};  // top-down: geotransform[5] < 0
    int width{};
    int height{};
};

// GDAL's own natural output grid for a plain "-t_srs <destWkt>" warp of the
// given source grid - the same GDALSuggestedWarpOutput query warpToCrsImpl
// already makes internally to size a DISPLAY warp, exposed here uncapped
// (no kMaxWarpDimension) so an export can keep native resolution. Throws
// UserError if GDAL cannot suggest one, e.g. the source domain lies outside
// the target CRS's area of use.
[[nodiscard]] DestinationGrid suggestWarpGrid(
    int width, int height, const std::string& sourceWkt, const std::array<double, 6>& sourceGeotransform, const std::string& destWkt);

// Builds a georeferenced in-memory GDAL dataset from a plain array and warps
// it to EPSG:3857, returning the warped array and its bounds - this is what
// makes the resulting overlay axis-aligned in tile space, the reason to
// reproject here rather than draw in the WRF file's native CRS (which would
// need a per-vertex/pixel reprojection in the paint path). Mirrors
// wrftools.rasterlayer._warp_to_web_mercator.
[[nodiscard]] WarpedRaster warpToWebMercator(std::span<const float> values, int width, int height,
    const std::string& sourceWkt, const std::array<double, 6>& sourceGeotransform);

// Mirrors rasterio.warp.Resampling's two modes w2w actually uses, plus the
// two this project's own display warp already uses - not the full GDAL set.
enum class ResampleMethod { Bilinear, Average, Mode, Nearest };

// Reprojects a raster into a CALLER-KNOWN destination grid (CRS +
// geotransform + size), unlike warpToWebMercator (which only ever targets
// EPSG:3857 and lets GDAL pick the output size). This is what the w2w LCZ
// port's per-UCP resamplers need: aggregating an LCZ-derived raster onto
// the exact WRF grid a geo_em file already defines, matching
// rasterio.warp.reproject(src, dst, ...)'s "already-shaped destination"
// semantics. Source NaNs are treated as nodata; destination pixels the
// source never covers come back NaN too.
[[nodiscard]] std::vector<float> warpToGrid(std::span<const float> values, int width, int height, const std::string& sourceWkt,
    const std::array<double, 6>& sourceGeotransform, const std::string& destWkt, const std::array<double, 6>& destGeotransform, int destWidth,
    int destHeight, ResampleMethod resampling);

// Reprojects into a target CRS with GDAL choosing the output size/
// geotransform on its own (unlike warpToGrid) - what w2w's
// check_lcz_integrity needs to normalize an LCZ GeoTIFF to EPSG:4326
// before any of the per-UCP resamplers touch it. Shares its
// implementation with warpToWebMercator internally (both let GDAL pick
// the output size for a plain "-t_srs <dest>" warp); warpToWebMercator is
// kept as its own function rather than a thin wrapper callers have to
// remember to parameterize, since EPSG:3857 + bilinear is its only ever
// use in this codebase.
[[nodiscard]] WarpedRaster warpToCrs(std::span<const float> values, int width, int height, const std::string& sourceWkt,
    const std::array<double, 6>& sourceGeotransform, const std::string& destWkt, ResampleMethod resampling);

}  // namespace wrftools
