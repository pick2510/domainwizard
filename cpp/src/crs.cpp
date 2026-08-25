#include "wrftools/crs.hpp"
#include "wrftools/error.hpp"

#include <ogr_spatialref.h>
#include <ogr_geometry.h>
#include <cpl_conv.h>

#include <algorithm>
#include <format>

namespace wrftools {
namespace {
// Deliberately not WGS84: WRF/WPS project on a perfect sphere. Using WGS84
// instead (as upstream GIS4WRF once did) makes cells ~0.3-0.6% anisotropic
// and shifts lon/lat placement by ~0.9 km on a 180 km domain. Matches
// gis4wrf.core.constants.WRF_EARTH_RADIUS / WRF_PROJ4_SPHERE.
constexpr double kWrfEarthRadius = 6370000.0;

std::string sphereSuffix() { return std::format(" +a={} +b={} +no_defs", kWrfEarthRadius, kWrfEarthRadius); }

OGRSpatialReference srsFromProj4(const std::string& proj4) {
    OGRSpatialReference srs;
    if (srs.importFromProj4(proj4.c_str()) != OGRERR_NONE) throw UserError("Could not build coordinate reference system from: " + proj4);
    return srs;
}

// A geographic CRS on the SAME ellipsoid/sphere as `srs`, with traditional
// (x, y) = (lon, lat) axis order forced - this is what to_xy/to_lonlat
// transform against, so no datum shift is ever applied. Mirrors
// CRS.lonlat_srs / fix_axis_order (crs.py:183-210): SetAxisMappingStrategy
// is what actually controls Transform() axis order, not EPSGTreatsAsLatLong
// (unreliable for a SetGeogCS-built CRS with no EPSG code).
OGRSpatialReference lonLatVariantOf(const OGRSpatialReference& srs) {
    OGRSpatialReference out;
    out.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    out.SetGeogCS("", srs.GetAttrValue("DATUM"), "", srs.GetSemiMajor(), srs.GetInvFlattening());
    return out;
}

Coordinate2D transformPoint(const OGRSpatialReference& from, const OGRSpatialReference& to, Coordinate2D point) {
    std::unique_ptr<OGRCoordinateTransformation> transform(OGRCreateCoordinateTransformation(&from, &to));
    if (!transform) throw UserError("Could not create coordinate transformation.");
    double x = point.x, y = point.y;
    if (!transform->Transform(1, &x, &y)) throw UserError("Coordinate transformation failed.");
    return {x, y};
}
}  // namespace

Crs Crs::lonLat() { return Crs("+proj=latlong" + sphereSuffix()); }

Crs Crs::wgs84() { return Crs("+proj=longlat +datum=WGS84 +no_defs"); }

Crs Crs::fromWkt(const std::string& wkt) {
    OGRSpatialReference srs;
    if (srs.importFromWkt(wkt.c_str()) != OGRERR_NONE) throw UserError("Could not parse coordinate reference system WKT.");
    char* proj4 = nullptr;
    if (srs.exportToProj4(&proj4) != OGRERR_NONE || !proj4) throw UserError("Could not convert coordinate reference system to proj4.");
    std::string result(proj4);
    CPLFree(proj4);
    return Crs(result);
}

Crs Crs::lambert(double trueLat1, double trueLat2, LonLat origin) {
    return Crs(std::format("+proj=lcc +lat_1={} +lat_2={} +lat_0={} +lon_0={} +x_0=0 +y_0=0{}",
        trueLat1, trueLat2, origin.lat, origin.lon, sphereSuffix()));
}

// Latitude of origin is always the equator.
Crs Crs::mercator(double trueLat1, double originLon) {
    return Crs(std::format("+proj=merc +lat_ts={} +lon_0={} +x_0=0 +y_0=0{}", trueLat1, originLon, sphereSuffix()));
}

Crs Crs::polar(double trueLat1, double originLon) {
    const double originLat = trueLat1 > 0 ? 90.0 : -90.0;
    return Crs(std::format("+proj=stere +lat_ts={} +lat_0={} +lon_0={} +x_0=0 +y_0=0{}", trueLat1, originLat, originLon, sphereSuffix()));
}

std::string Crs::wkt() const {
    auto srs = srsFromProj4(proj4_);
    char* value = nullptr;
    if (srs.exportToWkt(&value) != OGRERR_NONE || !value) throw UserError("Could not export coordinate reference system to WKT.");
    std::string result(value);
    CPLFree(value);
    return result;
}

Coordinate2D Crs::toXy(LonLat value) const {
    const auto srs = srsFromProj4(proj4_);
    const auto lonLatSrs = lonLatVariantOf(srs);
    return transformPoint(lonLatSrs, srs, {value.lon, value.lat});
}

LonLat Crs::toLonLat(Coordinate2D value) const {
    const auto srs = srsFromProj4(proj4_);
    const auto lonLatSrs = lonLatVariantOf(srs);
    const auto result = transformPoint(srs, lonLatSrs, value);
    return {result.x, result.y};
}

Bounds2D Crs::transformBbox(Bounds2D bounds, const Crs& target) const {
    const auto from = srsFromProj4(proj4_);
    const auto to = srsFromProj4(target.proj4_);
    const std::array<Coordinate2D, 4> corners{{
        transformPoint(from, to, {bounds.minX, bounds.minY}),
        transformPoint(from, to, {bounds.maxX, bounds.minY}),
        transformPoint(from, to, {bounds.minX, bounds.maxY}),
        transformPoint(from, to, {bounds.maxX, bounds.maxY}),
    }};
    Bounds2D result{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const auto& corner : corners) {
        result.minX = std::min(result.minX, corner.x); result.maxX = std::max(result.maxX, corner.x);
        result.minY = std::min(result.minY, corner.y); result.maxY = std::max(result.maxY, corner.y);
    }
    return result;
}

}  // namespace wrftools
