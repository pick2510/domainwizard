#include "wrftools/lcz.hpp"
#include "wrftools/error.hpp"
#include "wrftools/warp.hpp"

#include "third_party/nanoflann.hpp"

#include <netcdf.h>
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <set>

namespace wrftools {
namespace {

double globalAttr(const NetcdfFile& file, const std::string& name) {
    const auto attribute = file.getAttribute("", name);
    if (attribute.numbers.empty()) throw UserError("geo_em file has no numeric global attribute: " + name);
    return attribute.numbers.front();
}

std::size_t dimensionLength(const NetcdfFile& file, const std::string& name) {
    for (const auto& dimension : file.dimensions())
        if (dimension.name == name) return dimension.length;
    throw UserError("geo_em file has no dimension: " + name);
}

// A great-circle-chord point cloud over a lat/lon pixel grid, for
// nanoflann - mirrors using_kdtree's own ECEF-ish projection (w2w.py:432-
// 453) exactly: R=6371 (km; the unit is arbitrary since only relative
// distances matter for a k-nearest query), no ellipsoid correction.
struct EcefPointCloud {
    std::vector<std::array<double, 3>> points;

    explicit EcefPointCloud(const std::vector<float>& lat, const std::vector<float>& lon) {
        constexpr double kEarthRadiusKm = 6371.0;
        points.reserve(lat.size());
        for (std::size_t i = 0; i < lat.size(); ++i) {
            const double phi = static_cast<double>(lat[i]) * std::numbers::pi / 180.0;
            const double theta = static_cast<double>(lon[i]) * std::numbers::pi / 180.0;
            points.push_back({kEarthRadiusKm * std::cos(phi) * std::cos(theta), kEarthRadiusKm * std::cos(phi) * std::sin(theta),
                kEarthRadiusKm * std::sin(phi)});
        }
    }

    [[nodiscard]] std::size_t kdtree_get_point_count() const { return points.size(); }
    [[nodiscard]] double kdtree_get_pt(std::size_t idx, std::size_t dim) const { return points[idx][dim]; }
    template <class Bbox>
    bool kdtree_get_bbox(Bbox&) const {
        return false;
    }
};

using PixelKdTree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<double, EcefPointCloud>, EcefPointCloud, 3>;

// For every pixel, the indices of its `k` nearest pixels (by great-circle
// chord distance, ascending - including itself, always first at distance
// 0), matching using_kdtree's tree.query(..., k=kpoints) exactly.
std::vector<std::vector<std::uint32_t>> nearestNeighbors(const std::vector<float>& lat, const std::vector<float>& lon, std::size_t k) {
    EcefPointCloud cloud(lat, lon);
    PixelKdTree tree(3, cloud, nanoflann::KDTreeSingleIndexAdaptorParams(10));

    std::vector<std::vector<std::uint32_t>> result(cloud.points.size(), std::vector<std::uint32_t>(k));
    std::vector<double> distances(k);
    for (std::size_t i = 0; i < cloud.points.size(); ++i)
        tree.knnSearch(cloud.points[i].data(), static_cast<std::uint32_t>(k), result[i].data(), distances.data());
    return result;
}

// pandas Series.mode()[0]'s tie-break: the SMALLEST value among those
// tied for the highest frequency (mode() itself returns every tied value
// in ascending order; [0] takes the first).
float modalValue(const std::vector<float>& values) {
    std::map<int, int> counts;
    for (float v : values) ++counts[static_cast<int>(std::lround(v))];
    int bestCount = -1, bestValue = 0;
    for (const auto& [value, count] : counts)  // std::map iterates in ascending key order
        if (count > bestCount) { bestCount = count; bestValue = value; }
    return static_cast<float>(bestValue);
}

std::string canonicalWgs84Wkt() {
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* wkt = nullptr;
    if (srs.exportToWkt(&wkt) != OGRERR_NONE || !wkt) throw UserError("Could not build the EPSG:4326 CRS.");
    std::string result(wkt);
    CPLFree(wkt);
    return result;
}

}  // namespace

WrfGridInfo wrfGridInfo(const NetcdfFile& file) {
    const int mapProj = static_cast<int>(globalAttr(file, "MAP_PROJ"));
    const double trueLat1 = globalAttr(file, "TRUELAT1");
    const double trueLat2 = globalAttr(file, "TRUELAT2");
    const double standLon = globalAttr(file, "STAND_LON");
    const double moadCenLat = globalAttr(file, "MOAD_CEN_LAT");
    const double cenLon = globalAttr(file, "CEN_LON");
    const double cenLat = globalAttr(file, "CEN_LAT");
    const double dx = globalAttr(file, "DX");
    const double dy = globalAttr(file, "DY");

    // Projection ids match w2w.py:581-660 exactly (same as
    // wrf_file.cpp's buildWrfCrs, MAP_PROJ==6 aside - see lcz.hpp's
    // comment on why that one case can't reuse Crs::lonLat()).
    Crs wrfCrs = [&]() -> Crs {
        switch (mapProj) {
            case 1: return Crs::lambert(trueLat1, trueLat2, {standLon, moadCenLat});
            case 2: return Crs::polar(trueLat1, standLon);
            case 3: return Crs::mercator(trueLat1, standLon);
            case 6: return Crs::fromProj4(std::format("+proj=eqc +lon_0={} +x_0=0 +y_0=0 +a=6370000 +b=6370000 +no_defs", standLon));
            default: throw UnsupportedError("w2w: unsupported MAP_PROJ " + std::to_string(mapProj));
        }
    }();

    // e, n: the domain's projected center, transformed from its own
    // CEN_LON/CEN_LAT through real WGS84 - see WrfGridInfo's doc comment
    // for why this deliberately doesn't go through Crs::toXy.
    const auto origin = Crs::wgs84().transformBbox({cenLon, cenLat, cenLon, cenLat}, wrfCrs);
    const double e = origin.minX, n = origin.minY;

    const double nx = static_cast<double>(dimensionLength(file, "west_east"));
    const double ny = static_cast<double>(dimensionLength(file, "south_north"));

    const double x0 = -(nx - 1.0) / 2.0 * dx + e;
    const double y0 = -(ny - 1.0) / 2.0 * dy + n;

    // Affine.translation(x0 - dx/2, y0 - dy/2) * Affine.scale(dx, dy),
    // written out as a GDAL-style [c, a, b, f, d, e] geotransform. dy is
    // POSITIVE (not GDAL's usual negative row height) - see WrfGridInfo's
    // doc comment.
    const std::array<double, 6> geotransform{x0 - dx / 2.0, dx, 0.0, y0 - dy / 2.0, 0.0, dy};

    return {wrfCrs, geotransform, static_cast<int>(nx), static_cast<int>(ny)};
}

void removeUrban(const std::filesystem::path& srcPath, const std::filesystem::path& dstPath, int npixNlc, std::optional<int> npixArea) {
    auto src = NetcdfFile::open(srcPath, NetcdfFile::Mode::ReadOnly);

    const int origNumLandCat = static_cast<int>(globalAttr(src, "NUM_LAND_CAT"));
    const int urbanCat = static_cast<int>(globalAttr(src, "ISURBAN"));
    const int waterCat = static_cast<int>(globalAttr(src, "ISWATER"));
    const bool hasLakeCat = src.hasAttribute("", "ISLAKE") && [&] {
        const int islake = static_cast<int>(globalAttr(src, "ISLAKE"));
        return islake > 0 && islake < origNumLandCat;
    }();
    const int lakeCat = hasLakeCat ? static_cast<int>(globalAttr(src, "ISLAKE")) : 0;

    auto luse = src.readFloat("LU_INDEX");
    auto luf = src.readFloat("LANDUSEF");
    auto greenf = src.readFloat("GREENFRAC");
    const auto lat = src.readFloat("XLAT_M");
    const auto lon = src.readFloat("XLONG_M");

    const auto luShape = src.shape("LU_INDEX");      // {1, ny, nx}
    const auto greenfShape = src.shape("GREENFRAC");  // {1, months, ny, nx}
    const std::size_t ny = luShape[1], nx = luShape[2];
    const std::size_t npix = ny * nx;
    const std::size_t months = greenfShape[1];

    // Pre-pass (add_wrf_version only, absent from w2w's `main`): collapse
    // any LCZ-range LU_INDEX values the SOURCE file already carries back
    // to ISURBAN, so re-running this against a file w2w itself already
    // produced is idempotent. Ranges match w2w.py exactly, including its
    // own slight over-inclusiveness (the LANDUSEF slice's upper bound is
    // one past the LU_INDEX threshold's - see lcz.hpp's removeUrban doc).
    if (origNumLandCat == 61 || origNumLandCat == 41) {
        const int lo = origNumLandCat == 61 ? 50 : 30;
        const int lufHiExclusive = origNumLandCat == 61 ? 61 : 41;
        for (std::size_t p = 0; p < npix; ++p)
            if (luse[p] > static_cast<float>(lo) && luse[p] < static_cast<float>(origNumLandCat)) luse[p] = static_cast<float>(urbanCat);
        for (std::size_t p = 0; p < npix; ++p) {
            double sum = 0.0;
            for (int cat = lo; cat < lufHiExclusive; ++cat) sum += luf[static_cast<std::size_t>(cat) * npix + p];
            luf[static_cast<std::size_t>(urbanCat - 1) * npix + p] = static_cast<float>(sum);
        }
        for (int cat = lo; cat < lufHiExclusive; ++cat)
            for (std::size_t p = 0; p < npix; ++p) luf[static_cast<std::size_t>(cat) * npix + p] = 0.0f;
    }

    std::vector<float> newluse = luse;
    std::vector<float> newluf = luf;
    std::vector<float> newgreenf = greenf;

    std::vector<bool> lufNatland(npix), lufUrb(npix);
    for (std::size_t p = 0; p < npix; ++p) {
        const bool natland = luf[static_cast<std::size_t>(urbanCat - 1) * npix + p] == 0.0f &&
                              luf[static_cast<std::size_t>(waterCat - 1) * npix + p] == 0.0f &&
                              (!hasLakeCat || luf[static_cast<std::size_t>(lakeCat - 1) * npix + p] == 0.0f);
        lufNatland[p] = natland;
        lufUrb[p] = luf[static_cast<std::size_t>(urbanCat - 1) * npix + p] != 0.0f;
    }

    const std::size_t effectiveNpixArea = npixArea.value_or(static_cast<std::size_t>(npixNlc) * static_cast<std::size_t>(npixNlc));
    if (effectiveNpixArea > npix)
        throw UserError("The area you selected is larger than the domain size: you chose an area of " + std::to_string(effectiveNpixArea) +
                         " pixels and the domain is " + std::to_string(npix) + " pixels. Reduce NPIX_AREA.");
    const std::size_t kpoints = std::min(npix, effectiveNpixArea);
    const auto neighbors = nearestNeighbors(lat, lon, kpoints);

    std::vector<bool> luseUrb(npix), luseNatland(npix);
    for (std::size_t p = 0; p < npix; ++p) {
        luseUrb[p] = static_cast<int>(std::lround(luse[p])) == urbanCat;
        luseNatland[p] = static_cast<int>(std::lround(luse[p])) != urbanCat && static_cast<int>(std::lround(luse[p])) != waterCat &&
                          (!hasLakeCat || static_cast<int>(std::lround(luse[p])) != lakeCat);
    }

    for (std::size_t p = 0; p < npix; ++p) {
        if (!luseUrb[p]) continue;

        std::vector<std::uint32_t> auxKd;
        auxKd.reserve(static_cast<std::size_t>(npixNlc));
        for (std::uint32_t candidate : neighbors[p]) {
            if (!luseNatland[candidate]) continue;
            auxKd.push_back(candidate);
            if (auxKd.size() == static_cast<std::size_t>(npixNlc)) break;
        }
        if (auxKd.empty())
            throw UserError("Not enough natural land points in the selected area: sampled " + std::to_string(npixNlc) + " pixels over an area of " +
                             std::to_string(effectiveNpixArea) + " - increase NPIX_AREA.");

        std::vector<float> auxLuse;
        auxLuse.reserve(auxKd.size());
        for (std::uint32_t idx : auxKd) auxLuse.push_back(luse[idx]);
        const float mkd = modalValue(auxLuse);

        std::vector<double> greenSum(months, 0.0);
        int greenCount = 0;
        for (std::uint32_t idx : auxKd) {
            if (static_cast<int>(std::lround(luse[idx])) != static_cast<int>(std::lround(mkd))) continue;
            for (std::size_t m = 0; m < months; ++m) greenSum[m] += greenf[m * npix + idx];
            ++greenCount;
        }
        newluse[p] = mkd;
        for (std::size_t m = 0; m < months; ++m) newgreenf[m * npix + p] = static_cast<float>(greenSum[m] / greenCount);
    }

    for (std::size_t p = 0; p < npix; ++p) {
        if (!lufUrb[p]) continue;
        const int destCat = static_cast<int>(std::lround(newluse[p]));
        newluf[static_cast<std::size_t>(destCat - 1) * npix + p] += luf[static_cast<std::size_t>(urbanCat - 1) * npix + p];
        newluf[static_cast<std::size_t>(urbanCat - 1) * npix + p] = 0.0f;
    }

    NetcdfFile::copyFile(srcPath, dstPath);
    auto dst = NetcdfFile::open(dstPath, NetcdfFile::Mode::ReadWrite);
    dst.writeFloat("LU_INDEX", newluse);
    dst.writeFloat("LANDUSEF", newluf);
    dst.writeFloat("GREENFRAC", newgreenf);

    NetcdfFile::Attribute numLandCat;
    numLandCat.name = "NUM_LAND_CAT";
    numLandCat.type = NC_INT;
    numLandCat.numbers = {21.0};
    dst.putAttribute("", numLandCat);
}

LczRaster checkLczIntegrity(const std::filesystem::path& lczPath, int lczBand, const NetcdfFile& wrfFile) {
    GDALAllRegister();
    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> dataset(
        GDALDataset::FromHandle(GDALOpenEx(lczPath.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)),
        [](GDALDataset* d) { if (d) GDALClose(d); });
    if (!dataset) throw UserError("Cannot find LCZ map file: " + lczPath.string());
    if (lczBand < 0 || lczBand >= dataset->GetRasterCount())
        throw UserError("Cannot read the requested LCZ band " + std::to_string(lczBand) + " from the LCZ GeoTIFF - check the -l/--lcz-band argument.");

    const int width = dataset->GetRasterXSize(), height = dataset->GetRasterYSize();
    std::vector<float> values(static_cast<std::size_t>(width) * height);
    auto* band = dataset->GetRasterBand(lczBand + 1);  // GDAL bands are 1-indexed
    if (band->RasterIO(GF_Read, 0, 0, width, height, values.data(), width, height, GDT_Float32, 0, 0) != CE_None)
        throw UserError("Could not read the LCZ GeoTIFF: " + lczPath.string());
    // warpToCrs (like the rest of this pipeline) treats NaN as nodata, but
    // GDAL doesn't convert a band's registered NoData value to NaN on
    // read - do that ourselves so a real nodata value (e.g. -1, as
    // testing/Shanghai.tif uses) is actually excluded from the warp below,
    // rather than participating in nearest-neighbor resampling as if it
    // were real LCZ class data.
    int hasNodata = 0;
    const double nodata = band->GetNoDataValue(&hasNodata);
    if (hasNodata)
        for (auto& value : values)
            if (value == static_cast<float>(nodata)) value = std::numeric_limits<float>::quiet_NaN();

    std::array<double, 6> sourceGeotransform{};
    if (dataset->GetGeoTransform(sourceGeotransform.data()) != CE_None) throw UserError("LCZ GeoTIFF has no geotransform: " + lczPath.string());
    const std::string sourceWkt = dataset->GetProjectionRef() ? dataset->GetProjectionRef() : "";

    // _replace_lcz_number (w2w.py): if any 100-series (LCZ Generator)
    // class label is present, remap the WHOLE raster through
    // {1..10 -> 1..10, 101..107 -> 11..17} - matching w2w.py's own
    // dict-based remap, including that a value outside both ranges is
    // left unchanged here (Python's pandas .map() would instead turn it
    // into NaN and then undefined-behavior-cast that to int32, which
    // isn't a deliberate design choice worth reproducing).
    const bool has100Series = std::any_of(values.begin(), values.end(), [](float v) { return v >= 101.0f && v <= 107.0f; });
    if (has100Series) {
        std::map<int, int> remap;
        for (int v = 1; v <= 10; ++v) remap[v] = v;
        for (int v = 101; v <= 107; ++v) remap[v] = v - 101 + 11;
        for (auto& value : values) {
            if (std::isnan(value)) continue;
            const auto found = remap.find(static_cast<int>(std::lround(value)));
            if (found != remap.end()) value = static_cast<float>(found->second);
        }
    }

    const std::string wgs84Wkt = canonicalWgs84Wkt();
    OGRSpatialReference sourceSrs, wgs84Srs;
    sourceSrs.importFromWkt(sourceWkt.c_str());
    wgs84Srs.importFromEPSG(4326);
    const bool alreadyWgs84 = sourceSrs.IsSame(&wgs84Srs);

    std::array<double, 6> geotransform = sourceGeotransform;
    int finalWidth = width, finalHeight = height;
    if (!alreadyWgs84) {
        auto warped = warpToCrs(values, width, height, sourceWkt, sourceGeotransform, wgs84Wkt, ResampleMethod::Nearest);
        for (auto& value : warped.values)
            if (!(value > 0.0f)) value = 0.0f;  // xr.where(lcz.data > 0, lcz.data, 0) - also clears any NaN a partial-coverage warp left behind
        finalWidth = warped.width;
        finalHeight = warped.height;
        const double dx = (warped.bounds3857.maxX - warped.bounds3857.minX) / finalWidth;
        const double dy = (warped.bounds3857.maxY - warped.bounds3857.minY) / finalHeight;
        geotransform = {warped.bounds3857.minX, dx, 0.0, warped.bounds3857.maxY, 0.0, -dy};
        values = std::move(warped.values);
    }

    // _check_lcz_wrf_extent: the LCZ raster must cover the WRF domain's
    // own XLONG_M/XLAT_M bounds in every direction (strict inequality,
    // matching w2w.py exactly).
    const double lczXmin = geotransform[0], lczYmax = geotransform[3];
    const double lczXmax = lczXmin + geotransform[1] * finalWidth, lczYmin = lczYmax + geotransform[5] * finalHeight;
    const auto wrfLon = wrfFile.readFloat("XLONG_M");
    const auto wrfLat = wrfFile.readFloat("XLAT_M");
    const auto [wrfLonMinIt, wrfLonMaxIt] = std::minmax_element(wrfLon.begin(), wrfLon.end());
    const auto [wrfLatMinIt, wrfLatMaxIt] = std::minmax_element(wrfLat.begin(), wrfLat.end());
    const double wrfXmin = *wrfLonMinIt, wrfXmax = *wrfLonMaxIt, wrfYmin = *wrfLatMinIt, wrfYmax = *wrfLatMaxIt;
    if (!(wrfXmin > lczXmin && wrfXmax < lczXmax && wrfYmin > lczYmin && wrfYmax < lczYmax))
        throw UserError(std::format("LCZ domain should be larger than the WRF domain in all directions. LCZ bounds (xmin, ymin, xmax, ymax): "
                                     "({}, {}, {}, {}); WRF bounds (xmin, ymin, xmax, ymax): ({}, {}, {}, {})",
            lczXmin, lczYmin, lczXmax, lczYmax, wrfXmin, wrfYmin, wrfXmax, wrfYmax));

    return {std::move(values), finalWidth, finalHeight, wgs84Wkt, geotransform};
}

}  // namespace wrftools
