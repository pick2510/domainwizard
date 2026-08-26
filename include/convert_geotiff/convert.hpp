// Shared entry point for performing a GeoTIFF -> geogrid conversion, used
// by both the CLI and the GUI so the two never diverge in behavior.

#ifndef CONVERT_GEOTIFF_CONVERT_HPP
#define CONVERT_GEOTIFF_CONVERT_HPP

#include "convert_geotiff/geogrid_index.hpp"

#include <functional>
#include <string>

namespace convert_geotiff {

struct ConversionOptions {
  int border_width = 3;
  int word_size = 2;
  bool isigned = true;
  int tile_size = 100;
  float scale = 1.f;
  float missing = 0.f;
  std::string units = "\"NO UNITS\"";
  std::string description = "\"NO DESCRIPTION\"";
  int categorical_range = 0; // 0 = continuous data
};

// Called after each tile is written: (tile_x, tile_y, nx_tiles, ny_tiles).
using ProgressCallback = std::function<void(int, int, int, int)>;

// Reads `filename`, applies `opts`, and writes "index" plus every data tile
// to the current working directory. Returns the populated GeogridIndex.
// Throws GeoConvertError on failure.
GeogridIndex convert(const std::string &filename, const ConversionOptions &opts,
                      const ProgressCallback &on_tile = nullptr);

} // namespace convert_geotiff

#endif
