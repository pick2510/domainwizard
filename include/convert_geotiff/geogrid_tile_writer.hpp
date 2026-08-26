// Tiling and binary output logic for the geogrid format.

#ifndef CONVERT_GEOTIFF_GEOGRID_TILE_WRITER_HPP
#define CONVERT_GEOTIFF_GEOGRID_TILE_WRITER_HPP

#include "convert_geotiff/geogrid_index.hpp"

#include <vector>

namespace convert_geotiff {

// Applies dataset-wide processing to the buffer before tiling (e.g. mapping
// out-of-range categorical values to the missing value).
void process_buffer(const GeogridIndex &idx, std::vector<float> &databuf);

class GeogridTileWriter {
public:
  explicit GeogridTileWriter(const GeogridIndex &idx) : idx_(idx) {}

  int nx_tiles() const;
  int ny_tiles() const;
  int nz_size() const;

  // Global index of the first element of a tile (including border). May be
  // negative or beyond the image bounds; extract_tile() fills such
  // positions with the missing value.
  long tile_start(int itile_x, int itile_y) const;

  long global_y_stride() const;
  long global_z_stride() const;

  // Extracts one tile (including border) from the global data buffer.
  std::vector<float> extract_tile(int itile_x, int itile_y, const std::vector<float> &databuf) const;

  // Writes one tile to disk. Throws GeoConvertError on failure.
  void write_tile(int itile_x, int itile_y, const std::vector<float> &tile) const;

private:
  const GeogridIndex &idx_;
};

} // namespace convert_geotiff

#endif
