#include "wrftools/domain_overlay.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace wrftools {
namespace {
// Matches wrftools.domainoverlay._PALETTE exactly.
constexpr std::array<std::array<int, 3>, 8> kPalette{{
    {220, 30, 30}, {30, 90, 220}, {30, 160, 60}, {230, 140, 20},
    {150, 40, 190}, {20, 160, 160}, {190, 30, 130}, {140, 120, 20},
}};

QColor penColorForDomainNumber(int domainNumber) {
    const auto& c = kPalette[static_cast<std::size_t>(domainNumber - 1) % kPalette.size()];
    return QColor(c[0], c[1], c[2]);
}

// Subdivides a rectangle's ring into segments no longer than
// perimeter/200, in projected space, then projects each vertex to lon/lat -
// so the resulting polyline follows the true projection curvature instead
// of just connecting 4 corners with straight lines. Mirrors
// domainoverlay.py's `geom.Segmentize(perimeter / 200.0)` (done before
// reprojecting to WGS84).
std::vector<LonLat> densifiedRing(const Bounds2D& bbox, const Crs& projection) {
    const std::array<Coordinate2D, 5> ring{{
        {bbox.minX, bbox.minY}, {bbox.maxX, bbox.minY}, {bbox.maxX, bbox.maxY}, {bbox.minX, bbox.maxY}, {bbox.minX, bbox.minY},
    }};
    const double perimeter = 2.0 * (bbox.maxX - bbox.minX) + 2.0 * (bbox.maxY - bbox.minY);
    const double maxSegment = perimeter > 0 ? perimeter / 200.0 : 1.0;
    std::vector<LonLat> points;
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
        const auto& a = ring[i]; const auto& b = ring[i + 1];
        const double edgeLength = std::hypot(b.x - a.x, b.y - a.y);
        const int segments = std::max(1, static_cast<int>(std::ceil(edgeLength / maxSegment)));
        for (int s = 0; s < segments; ++s) {
            const double t = static_cast<double>(s) / segments;
            points.push_back(projection.toLonLat({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}));
        }
    }
    points.push_back(projection.toLonLat(ring.back()));
    return points;
}
}  // namespace

std::vector<VectorOverlay> computeDomainOverlays(DomainProject& project) {
    if (project.domains().empty()) return {};
    try {
        project.fillDomains();
    } catch (const UserError&) {
        return {};
    }
    const auto projection = project.projection();
    std::vector<VectorOverlay> overlays;
    overlays.reserve(project.domains().size());
    for (const auto& domain : project.domains()) {
        if (!domain.bounds) continue;
        overlays.push_back({densifiedRing(*domain.bounds, projection), penColorForDomainNumber(domain.id), 2.0, /*closed=*/true});
    }
    return overlays;
}

std::optional<std::pair<LonLat, LonLat>> domainLonLatBounds(const std::vector<VectorOverlay>& overlays) {
    if (overlays.empty()) return std::nullopt;
    double minLon = std::numeric_limits<double>::infinity(), minLat = std::numeric_limits<double>::infinity();
    double maxLon = -std::numeric_limits<double>::infinity(), maxLat = -std::numeric_limits<double>::infinity();
    for (const auto& overlay : overlays) for (const auto& point : overlay.points) {
        minLon = std::min(minLon, point.lon); maxLon = std::max(maxLon, point.lon);
        minLat = std::min(minLat, point.lat); maxLat = std::max(maxLat, point.lat);
    }
    return std::make_pair(LonLat{minLon, minLat}, LonLat{maxLon, maxLat});
}

}  // namespace wrftools
