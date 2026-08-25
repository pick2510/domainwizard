#pragma once

#include "wrftools/domain.hpp"

#include <filesystem>
#include <string>

namespace wrftools {

// Projection and the root's reference point live on domains.domains()[0]
// (mapProj/trueLat1/trueLat2/standLon/centerLon/centerLat) - WPS defines one
// projection per project regardless of nesting shape, same as gis4wrf.core.
struct WpsProject {
    DomainProject domains;
};

// Supports the WPS domain subset used by WRF Tools. Unknown namelist groups
// are intentionally ignored, matching the Python import's best-effort use.
[[nodiscard]] WpsProject readWpsNamelist(const std::filesystem::path& path);
void writeWpsNamelist(const WpsProject& project, const std::filesystem::path& path);

}  // namespace wrftools
