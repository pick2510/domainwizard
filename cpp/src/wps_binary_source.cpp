#include "wrftools/wps_binary_source.hpp"
#include "wrftools/crs.hpp"
#include "wrftools/error.hpp"

#include "convert_geotiff/geogrid_index.hpp"
#include "convert_geotiff/geogrid_reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wrftools {
namespace {
std::string stripQuotes(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
    return value;
}

// Origin latitude is not recorded in a WPS_GEOG index file for any
// projected CRS (only truelat1/2 and stdlon are) - truelat1 is used as a
// fixed, self-consistent choice, mirroring convert_geotiff's own
// geotiff_writer.cpp (ProjFalseOriginLatGeoKey/ProjNatOriginLatGeoKey).
// Since the tie point below is projected through this same CRS instance,
// this arbitrary choice cancels out and never affects where a pixel ends
// up on the ground.
Crs buildGeogCrs(const convert_geotiff::GeogridIndex& idx) {
    using convert_geotiff::Projection;
    switch (idx.proj) {
        case Projection::Lambert: return Crs::lambert(idx.truelat1, idx.truelat2, {idx.stdlon, idx.truelat1});
        case Projection::Polar: return Crs::polar(idx.truelat1, idx.stdlon);
        case Projection::Mercator: return Crs::mercator(idx.truelat1, idx.stdlon);
        case Projection::RegularLL: return Crs::lonLat();
        case Projection::AlbersNad83: throw UnsupportedError("WPS_GEOG albers_nad83 datasets are not supported.");
    }
    throw UnsupportedError("Unsupported WPS_GEOG projection.");
}
}  // namespace

bool isWpsGeogDataset(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && std::filesystem::is_regular_file(path / "index", error);
}

WpsBinarySource::WpsBinarySource(std::filesystem::path directory) : directory_(std::move(directory)) {
    convert_geotiff::GeogridIndex idx;
    std::vector<float> raw;
    try {
        idx = convert_geotiff::read_index_file(directory_.string());
        raw = convert_geotiff::read_tiles(directory_.string(), idx);
    } catch (const std::exception& error) {
        throw UserError("Could not read WPS_GEOG dataset '" + directory_.string() + "': " + error.what());
    }

    const auto crs = buildGeogCrs(idx);
    projectionWkt_ = crs.wkt();

    const int rawNx = idx.nx, rawNy = idx.ny;
    const int nz = idx.nz > 0 ? idx.nz : (idx.tz_e - idx.tz_s + 1);
    if (rawNx <= 0 || rawNy <= 0 || nz <= 0) throw UserError("WPS_GEOG dataset has an invalid size: " + directory_.string());

    // Some real-world global regular_ll datasets pad nx/ny out to a whole
    // number of tiles beyond the true 360deg/180deg extent (observed in
    // NCAR's own topo_gmted2010_5m: tile_x=600 doesn't divide 4320 evenly,
    // so it ships 4800 columns instead - the trailing 480 are a literal
    // duplicate of columns 0..479, and similarly 240 trailing rows of
    // zero-filled padding past the pole). Left in, those extra columns
    // either get wrongly treated as "not quite a full globe" (skipping the
    // wraparound fix below entirely) or the zero-filled rows get warped as
    // if they were real data. Crop back to the true size first - the
    // padding is always appended at the high-index end, regardless of
    // which physical direction that raw index happens to represent, since
    // it comes from read_tiles() rounding up to whole tiles.
    int nx = rawNx, ny = rawNy;
    if (idx.proj == convert_geotiff::Projection::RegularLL) {
        if (idx.dx > 0.f) {
            const int trueNx = static_cast<int>(std::lround(360.0 / idx.dx));
            if (trueNx > 0 && rawNx > trueNx) nx = trueNx;
        }
        if (idx.dy > 0.f) {
            const int trueNy = static_cast<int>(std::lround(180.0 / idx.dy));
            if (trueNy > 0 && rawNy > trueNy) ny = trueNy;
        }
    }
    size_ = {nx, ny};

    // Same tie-point convention as convert_geotiff's own geotiff_writer.cpp
    // (GeoTIFF RasterPixelIsArea, raster row 0 = north): a tile file's own
    // row 1 is south when dy is non-negative (this port's own convert.cpp
    // always emits that), north when dy is negative (an idiosyncratic
    // real-world index-file authoring choice geotiff_writer.cpp's own
    // comment documents observing) - flip only in the first case to reach
    // row-0-is-north.
    const bool flipRows = idx.dy >= 0.f;
    const auto known = crs.toXy({idx.known_lon, idx.known_lat});
    const double pixelWidth = std::abs(static_cast<double>(idx.dx));
    const double pixelHeight = std::abs(static_cast<double>(idx.dy));
    // known_x/known_y locate the CENTER of a grid cell (WPS/GEOGRID's
    // documented convention for the common integer case, e.g. known_x=1.0
    // means "center of column 1") - not a pixel corner, so an extra
    // half-pixel step from the tie pixel's center out to its west/north
    // edge is needed on top of the (known_x - 1)/(known_y - 1) pixel
    // offset. Omitting it silently shifts the whole raster by half a cell
    // (a real-world 1-degree global dataset then spans -90.5..89.5 instead
    // of -90..90, which is invalid latitude for the web-Mercator warp).
    const double tiePixelX = idx.known_x - 1;
    const double tiePixelY = flipRows ? (ny - idx.known_y) : (idx.known_y - 1);
    geotransform_ = {known.x - (tiePixelX + 0.5) * pixelWidth, pixelWidth, 0.0, known.y + (tiePixelY + 0.5) * pixelHeight, 0.0, -pixelHeight};

    // Only a lon/lat (RegularLL) dataset has this ambiguity - a projected
    // CRS's coordinates are in meters and don't wrap. Some real-world
    // global WPS_GEOG datasets (e.g. GMTED2010) record known_lon in a
    // 0..360 convention rather than -180..180; left as-is, the part past
    // +180 projects to web-Mercator x values far outside the map's valid
    // range instead of wrapping back to the western hemisphere, which
    // looks like the raster simply stopping partway around the globe.
    int columnShift = 0;
    if (idx.proj == convert_geotiff::Projection::RegularLL) {
        const double rawXMin = geotransform_[0];
        double normalizedXMin = rawXMin;
        while (normalizedXMin > 180.0 + 1e-6) normalizedXMin -= 360.0;
        while (normalizedXMin < -180.0 - 1e-6) normalizedXMin += 360.0;
        const double lonSpan = nx * pixelWidth;
        if (normalizedXMin + lonSpan <= 180.0 + 1e-6) {
            // Fits in -180..180 once shifted by a whole number of 360s - no
            // reordering needed, the raster just wasn't centered on the
            // standard range (e.g. a regional dataset given as 200..210
            // instead of -160..-150).
            geotransform_[0] = normalizedXMin;
        } else if (std::abs(lonSpan - 360.0) < pixelWidth) {
            // A full-globe raster: cyclic-shift columns so the seam moves
            // from wherever known_lon happened to put it to exactly
            // +-180, and re-express the west edge as -180. Column j's
            // source column i satisfies known_lon + i*dx = -180 + j*dx
            // (mod 360), i.e. i = (j - shift) mod nx with
            // shift = round((180 + rawXMin) / dx).
            columnShift = static_cast<int>(std::lround((180.0 + rawXMin) / pixelWidth)) % nx;
            if (columnShift < 0) columnShift += nx;
            geotransform_[0] = -180.0;
        }
        // Else: a partial dataset that itself straddles the seam even after
        // normalizing (rare) is left as-is rather than guessing how to split it.
    }

    {
        const auto xMin = geotransform_[0], yMax = geotransform_[3];
        const auto xMax = xMin + geotransform_[1] * nx, yMin = yMax + geotransform_[5] * ny;
        const std::array<LonLat, 4> corners{crs.toLonLat({xMin, yMin}), crs.toLonLat({xMax, yMin}), crs.toLonLat({xMin, yMax}), crs.toLonLat({xMax, yMax})};
        geographicBounds_ = {.west = corners[0].lon, .south = corners[0].lat, .east = corners[0].lon, .north = corners[0].lat};
        for (const auto& corner : corners) {
            geographicBounds_.west = std::min(geographicBounds_.west, corner.lon); geographicBounds_.east = std::max(geographicBounds_.east, corner.lon);
            geographicBounds_.south = std::min(geographicBounds_.south, corner.lat); geographicBounds_.north = std::max(geographicBounds_.north, corner.lat);
        }
    }

    // Flip rows to top-down and drop the "missing" sentinel to NaN,
    // matching WrfFile::read()'s own NaN convention for the rest of the
    // render pipeline (colorizeWarped/warpToWebMercator).
    buffer_.assign(static_cast<std::size_t>(nx) * ny * nz, 0.0f);
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            const int srcRow = flipRows ? (ny - 1 - y) : y;
            for (int x = 0; x < nx; ++x) {
                const int srcCol = columnShift == 0 ? x : ((x - columnShift) % nx + nx) % nx;
                // raw is indexed with the uncropped stride (rawNx/rawNy) -
                // srcRow/srcCol are always within [0, ny)/[0, nx), which is
                // a subset of the raw extent, so this never touches the
                // padding rows/columns cropped above.
                const float value = raw[(static_cast<std::size_t>(z) * rawNy + srcRow) * rawNx + srcCol];
                buffer_[(static_cast<std::size_t>(z) * ny + y) * nx + x] = value == idx.missing ? std::numeric_limits<float>::quiet_NaN() : value;
            }
        }
    }

    WrfVariable variable;
    variable.name = stripQuotes(idx.description);
    if (variable.name.empty()) variable.name = directory_.filename().string();
    variable.description = variable.name;
    variable.units = stripQuotes(idx.units);
    variable.timeCount = 1;
    variable.levelCount = nz;
    // No named scheme is recorded in a WPS_GEOG index file - categoricalLut
    // falls back to its deterministic 8-color cycle + "Category N" labels
    // for an unrecognized (here, empty) scheme, same as WrfFile does for
    // e.g. ISLTYP.
    if (idx.categorical) variable.categoryScheme = "";
    variables_.push_back(std::move(variable));
}

std::string WpsBinarySource::displayName() const { return directory_.filename().string(); }

std::vector<float> WpsBinarySource::read(const std::string& variable, int timeIndex, int levelIndex) {
    if (variables_.empty() || variable != variables_.front().name || timeIndex != 0 || levelIndex < 0 || levelIndex >= variables_.front().levelCount)
        throw UserError("Variable or level index is not available: " + variable);
    const auto sliceSize = static_cast<std::size_t>(size_[0]) * size_[1];
    const auto offset = static_cast<std::size_t>(levelIndex) * sliceSize;
    return {buffer_.begin() + static_cast<std::ptrdiff_t>(offset), buffer_.begin() + static_cast<std::ptrdiff_t>(offset + sliceSize)};
}

}  // namespace wrftools
