// Writes a GeogridIndex + reassembled data buffer out as a GeoTIFF file --
// the inverse of GeoTiffFile (geotiff_reader.hpp).

#ifndef CONVERT_GEOTIFF_GEOTIFF_WRITER_HPP
#define CONVERT_GEOTIFF_GEOTIFF_WRITER_HPP

#include "convert_geotiff/geogrid_index.hpp"

#include <string>
#include <vector>

namespace convert_geotiff {

// Writes `buffer` (idx.nx * idx.ny * nz row-major floats, top-to-bottom row
// order) to a new GeoTIFF file at `path`, with georeferencing derived from
// `idx`. Throws GeoConvertError on failure.
void write_geotiff(const std::string &path, const GeogridIndex &idx, const std::vector<float> &buffer);

} // namespace convert_geotiff

#endif
