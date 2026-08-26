#include "convert_geotiff/convert.hpp"

#include "convert_geotiff/geogrid_tile_writer.hpp"
#include "convert_geotiff/geotiff_reader.hpp"

#include <algorithm>
#include <vector>

namespace convert_geotiff {

GeogridIndex convert(const std::string &filename, const ConversionOptions &opts,
                      const ProgressCallback &on_tile) {
  GeoTiffFile file(filename);

  GeogridIndex idx = file.get_index();

  idx.description = opts.description;
  idx.units = opts.units;
  idx.missing = opts.missing;
  if (opts.categorical_range) {
    idx.categorical = true;
    idx.cat_max = opts.categorical_range + 1;
    idx.cat_min = 1;
    idx.missing = static_cast<float>(idx.cat_max);
  } else {
    idx.categorical = false;
  }

  idx.tile_bdr = opts.border_width;
  idx.wordsize = opts.word_size;
  idx.isigned = opts.isigned;
  idx.tx = opts.tile_size;
  idx.ty = opts.tile_size;
  idx.scalefactor = opts.scale;

  if (idx.nx > 99999 - idx.tx || idx.ny > 99999 - idx.ty) {
    throw GeoConvertError("The data set is too large for geogrid format!");
  }

  idx.write_index_file("index");

  std::vector<float> buffer = file.read_buffer();

  if (!idx.bottom_top) {
    // Buffer is z-major (each z-level/band occupies a contiguous nx*ny
    // plane, matching GeogridTileWriter's z-stride convention); flip each
    // plane independently.
    const int nz = (idx.nz > 0) ? idx.nz : (idx.tz_e - idx.tz_s + 1);
    const size_t plane = static_cast<size_t>(idx.nx) * idx.ny;
    for (int z = 0; z < nz; ++z) {
      const size_t base = static_cast<size_t>(z) * plane;
      for (int i = 0; i < idx.ny / 2; ++i) {
        for (int j = 0; j < idx.nx; ++j) {
          std::swap(buffer[base + static_cast<size_t>(i) * idx.nx + j],
                    buffer[base + static_cast<size_t>(idx.ny - i - 1) * idx.nx + j]);
        }
      }
    }
    idx.bottom_top = true;
  }

  process_buffer(idx, buffer);

  GeogridTileWriter writer(idx);
  const int nxt = writer.nx_tiles();
  const int nyt = writer.ny_tiles();
  for (int itile_y = 0; itile_y < nyt; ++itile_y) {
    for (int itile_x = 0; itile_x < nxt; ++itile_x) {
      std::vector<float> tile = writer.extract_tile(itile_x, itile_y, buffer);
      writer.write_tile(itile_x, itile_y, tile);
      if (on_tile) on_tile(itile_x, itile_y, nxt, nyt);
    }
  }

  return idx;
}

} // namespace convert_geotiff
