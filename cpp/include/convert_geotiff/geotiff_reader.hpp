// RAII wrapper around an open GeoTIFF file, plus the logic to populate a
// GeogridIndex from its tags and read its pixel data.

#ifndef CONVERT_GEOTIFF_GEOTIFF_READER_HPP
#define CONVERT_GEOTIFF_GEOTIFF_READER_HPP

#include "convert_geotiff/geogrid_index.hpp"

#include <geotiffio.h>
#include <xtiffio.h>
#include <tiffio.h>

#include <string>
#include <vector>

namespace convert_geotiff {

class GeoTiffFile {
public:
  // Opens `path` for reading. Throws GeoConvertError if the file cannot be
  // opened.
  explicit GeoTiffFile(const std::string &path);

  GeoTiffFile(const GeoTiffFile &) = delete;
  GeoTiffFile &operator=(const GeoTiffFile &) = delete;

  ~GeoTiffFile();

  // Populates a GeogridIndex from the file's GeoTIFF/TIFF tags. Throws
  // GeoConvertError if required tags are missing or unsupported.
  GeogridIndex get_index() const;

  // Reads the raster's pixel data into a row-major float buffer of size
  // nx*ny*nz (in native TIFF row order -- top-to-bottom unless the
  // orientation tag says otherwise). Throws GeoConvertError on I/O errors
  // or unsupported pixel formats.
  std::vector<float> read_buffer() const;

private:
  TIFF *tif_ = nullptr;
};

} // namespace convert_geotiff

#endif
