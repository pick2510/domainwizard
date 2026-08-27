#pragma once

#include "wrftools/crs.hpp"
#include "wrftools/netcdf_file.hpp"

#include <array>
#include <cstddef>

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

}  // namespace wrftools
