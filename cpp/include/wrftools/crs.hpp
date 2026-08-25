#pragma once

#include <string>

namespace wrftools {

struct Coordinate2D {
    double x{};
    double y{};
};
struct LonLat {
    double lon{};
    double lat{};
};
struct Bounds2D {
    double minX{};
    double minY{};
    double maxX{};
    double maxY{};
};

// A WRF/WPS coordinate reference system, built from a proj4 string on the
// WRF sphere (a=b=6370000 m, not WGS84 - see crs.cpp). Mirrors
// gis4wrf.core.crs.CRS: transforms are always against this CRS's own
// lon/lat variant (same ellipsoid) so no datum shift can sneak in.
class Crs {
public:
    [[nodiscard]] static Crs lonLat();
    [[nodiscard]] static Crs lambert(double trueLat1, double trueLat2, LonLat origin);
    [[nodiscard]] static Crs mercator(double trueLat1, double originLon);
    [[nodiscard]] static Crs polar(double trueLat1, double originLon);
    // Real WGS84 (EPSG:4326), traditional (lon, lat) axis order - distinct
    // from lonLat() (the WRF-sphere lon/lat variant): this is what a map
    // widget's on-screen lon/lat and most external files' extents are in.
    [[nodiscard]] static Crs wgs84();
    // Wraps an externally-supplied CRS (e.g. a file's own projection, read
    // via GDAL) given as WKT, for use as the source of a transformBbox call.
    [[nodiscard]] static Crs fromWkt(const std::string& wkt);

    [[nodiscard]] const std::string& proj4() const noexcept { return proj4_; }
    [[nodiscard]] std::string wkt() const;

    [[nodiscard]] Coordinate2D toXy(LonLat value) const;
    [[nodiscard]] LonLat toLonLat(Coordinate2D value) const;
    [[nodiscard]] Bounds2D transformBbox(Bounds2D bounds, const Crs& target) const;

private:
    explicit Crs(std::string proj4) : proj4_(std::move(proj4)) {}
    std::string proj4_;
};

}  // namespace wrftools
