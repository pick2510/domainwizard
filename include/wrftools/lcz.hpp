#pragma once

#include "wrftools/crs.hpp"
#include "wrftools/netcdf_file.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>

namespace wrftools {

// The WRF grid a geo_em file defines, as a CRS + a GDAL-style 6-term
// geotransform - ported from w2w's own _get_wrf_grid_info
// (w2w/w2w.py:581-660 on the add_wrf_version branch, see PORT_W2W.MD Stage
// 1), NOT the same geotransform WrfFile derives elsewhere in this project.
// Two real differences from WrfFile's own geotransform:
//   - WrfFile reads through GDAL's NetCDF driver, which silently flips
//     rows to present a north-up (top-down) array; NetcdfFile (used here)
//     reads the raw on-disk order directly, matching what w2w's own
//     xr.open_dataset sees - WRF's own south_north dimension increases
//     NORTHWARD (row 0 is the southernmost row), so the geotransform below
//     has a POSITIVE row-height term, not GDAL's usual negative one. Any
//     data warped through this geotransform (see warpToGrid) must come
//     from the same raw, un-flipped row order - never from WrfFile.
//   - The projected origin (e, n) is computed by transforming the file's
//     own CEN_LON/CEN_LAT through REAL WGS84 (Crs::wgs84()), not through
//     this project's usual no-datum-shift WRF-sphere lon/lat variant
//     (Crs::toXy) - matching w2w's own pyproj.Proj(proj='latlong',
//     datum='WGS84') source CRS exactly, since that's what values are
//     pinned against.
struct WrfGridInfo {
    Crs crs;
    std::array<double, 6> geotransform{};
    int width{};   // west_east
    int height{};  // south_north
};

[[nodiscard]] WrfGridInfo wrfGridInfo(const NetcdfFile& file);

// Ports w2w's wrf_remove_urban (w2w.py, add_wrf_version branch - see
// PORT_W2W.MD Stage 2): replaces every urban LU_INDEX pixel with the
// dominant land-use category among its nearest natural-land neighbors
// (great-circle nearest-neighbor search, matching using_kdtree exactly -
// R=6371 km ECEF projection), moves each pixel's urban LANDUSEF fraction
// into the resulting category, and averages GREENFRAC over the neighbors
// sharing that category. Also collapses any LCZ-range LU_INDEX values the
// SOURCE file may already carry (orig NUM_LAND_CAT 41 or 61) back to
// ISURBAN first, so re-running this against a file w2w itself already
// produced is idempotent - new in add_wrf_version, absent from w2w's
// `main`.
//
// `srcPath` is read but never modified; the result (a full copy of
// `srcPath` with LU_INDEX/LANDUSEF/GREENFRAC replaced and NUM_LAND_CAT set
// to 21) is written to `dstPath`, overwriting it if present - mirrors
// wrf_remove_urban always regenerating info.dst_nu_file from info.dst_file
// each run, not modifying dst_file in place.
//
// npixArea defaults to npixNlc*npixNlc when absent, matching
// args.NPIX_AREA's own default. Throws UserError if npixArea exceeds the
// domain's pixel count, or if any urban pixel has zero natural-land
// neighbors within its npixArea-nearest set (both raise-and-exit in
// Python; this project's convention is to throw instead).
void removeUrban(const std::filesystem::path& srcPath, const std::filesystem::path& dstPath, int npixNlc, std::optional<int> npixArea = std::nullopt);

}  // namespace wrftools
