// Reads a geogrid dataset (an "index" metadata file plus its binary data
// tiles) back into a GeogridIndex and a reassembled float buffer. The
// exact inverse of GeogridIndex::write_index_file() and
// GeogridTileWriter's tile writing (see geogrid_index.hpp/geogrid_tile_writer.hpp).

#ifndef CONVERT_GEOTIFF_GEOGRID_READER_HPP
#define CONVERT_GEOTIFF_GEOGRID_READER_HPP

#include "convert_geotiff/geogrid_index.hpp"

#include <string>
#include <vector>

namespace convert_geotiff {

// Parses `dir`/index. Does not set nx/ny (not recorded in the index file --
// see read_tiles()). Throws GeoConvertError on a missing/malformed file.
GeogridIndex read_index_file(const std::string &dir);

// Scans `dir` for geogrid tile files, validates that a complete
// rectangular set is present, fills in idx.nx/idx.ny (recovered from the
// tile filenames -- see geogrid_reader.cpp), and returns the reassembled
// row-major float buffer (idx.nx * idx.ny * nz, top-to-bottom row order).
// Throws GeoConvertError on any missing/malformed tile.
std::vector<float> read_tiles(const std::string &dir, GeogridIndex &idx);

} // namespace convert_geotiff

#endif
