#pragma once

#include "wrftools/domain.hpp"

#include <filesystem>
#include <string>

namespace wrftools {

struct WpsProject {
    DomainProject domains;
    std::string mapProjection;
    double referenceLongitude{};
    double referenceLatitude{};
    double trueLatitude1{};
    double trueLatitude2{};
    double standardLongitude{};
};

// Supports the WPS domain subset used by WRF Tools. Unknown namelist groups
// are intentionally ignored, matching the Python import's best-effort use.
[[nodiscard]] WpsProject readWpsNamelist(const std::filesystem::path& path);
void writeWpsNamelist(const WpsProject& project, const std::filesystem::path& path);

}  // namespace wrftools
