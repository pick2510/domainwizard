#pragma once

#include "wrftools/crs.hpp"

#include <filesystem>

namespace wrftools {

struct FileExtent {
    Bounds2D bounds;
    Crs crs;
};

// Reads a file's extent and CRS - a raster's geotransform corners, or a
// vector layer's GetExtent() - for the Domains tab's "Set from File..."
// action. Throws UserError if the file can't be opened as either, or a
// vector layer has no spatial reference defined. Mirrors
// wrftools.fileextent.read_extent_and_srs.
[[nodiscard]] FileExtent readFileExtent(const std::filesystem::path& path);

}  // namespace wrftools
