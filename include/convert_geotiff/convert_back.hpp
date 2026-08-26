// Shared entry point for the reverse conversion (geogrid -> GeoTIFF), used
// by both the CLI and the GUI. Mirrors convert.hpp's role for the forward
// direction.

#ifndef CONVERT_GEOTIFF_CONVERT_BACK_HPP
#define CONVERT_GEOTIFF_CONVERT_BACK_HPP

#include <string>

namespace convert_geotiff {

// Reads the geogrid dataset in `geogrid_dir` (an "index" file plus its
// binary tiles) and writes it out as a GeoTIFF at `output_tiff`. Throws
// GeoConvertError on failure.
void convert_back(const std::string &geogrid_dir, const std::string &output_tiff);

} // namespace convert_geotiff

#endif
