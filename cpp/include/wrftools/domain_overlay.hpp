#pragma once

#include "wrftools/domain.hpp"
#include "wrftools/tile_map_widget.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace wrftools {

// Computes one closed, densified VectorOverlay ring per domain (in WGS84
// lon/lat, ready for TileMapWidget), colored from an 8-entry palette cycled
// by the domain's stable WPS number - so any number of siblings at any
// depth stay visually distinguishable, not just "innermost vs. everything
// else". Calls DomainProject::fillDomains() internally; returns {} (rather
// than throwing) if the project isn't fully configured yet, so a caller can
// clear the map's outline group on a UserError instead of crashing.
// Mirrors wrftools.domainoverlay.compute_domain_overlays.
[[nodiscard]] std::vector<VectorOverlay> computeDomainOverlays(DomainProject& project);

// Bounds (southWest, northEast) over every densified vertex of every
// domain's outline - not just the 4 corners, so the result covers the true
// curved outline in a projected CRS. std::nullopt if `overlays` is empty.
// Mirrors wrftools.domainoverlay.domain_lonlat_bounds.
[[nodiscard]] std::optional<std::pair<LonLat, LonLat>> domainLonLatBounds(const std::vector<VectorOverlay>& overlays);

}  // namespace wrftools
