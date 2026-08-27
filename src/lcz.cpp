#include "wrftools/lcz.hpp"
#include "wrftools/error.hpp"

#include <format>

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

}  // namespace wrftools
