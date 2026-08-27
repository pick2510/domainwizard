#include "wrftools/lcz.hpp"
#include "wrftools/error.hpp"

#include "third_party/nanoflann.hpp"

#include <netcdf.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numbers>

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

}  // namespace wrftools
