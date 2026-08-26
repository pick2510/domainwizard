#include "convert_geotiff/convert_back.hpp"

#include "convert_geotiff/geogrid_reader.hpp"
#include "convert_geotiff/geotiff_writer.hpp"

#include <vector>

namespace convert_geotiff {

void convert_back(const std::string &geogrid_dir, const std::string &output_tiff) {
  GeogridIndex idx = read_index_file(geogrid_dir);
  std::vector<float> buffer = read_tiles(geogrid_dir, idx);
  write_geotiff(output_tiff, idx, buffer);
}

} // namespace convert_geotiff
