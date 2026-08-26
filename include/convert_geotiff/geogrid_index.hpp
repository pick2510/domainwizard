// Metadata describing a geogrid dataset, and the exception type used
// throughout the library instead of fprintf+exit.

#ifndef CONVERT_GEOTIFF_GEOGRID_INDEX_HPP
#define CONVERT_GEOTIFF_GEOGRID_INDEX_HPP

#include <stdexcept>
#include <string>

namespace convert_geotiff {

class GeoConvertError : public std::runtime_error {
public:
  explicit GeoConvertError(const std::string &what) : std::runtime_error(what) {}
};

enum class Projection {
  Lambert,     // Lambert Conformal (geogrid code = PROJ_LC)
  Polar,       // Polar Stereographic (geogrid code = PROJ_PS)
  Mercator,    // Mercator (geogrid code = PROJ_MERC)
  RegularLL,   // Cylindrical (geographic) Lat/Lon (geogrid code = PROJ_LATLON)
  AlbersNad83  // Albers Equal Area Conic (geogrid code = PROJ_ALBERS_NAD83)
};

// Metadata written to the geogrid "index" file, plus everything needed to
// tile and write the binary data.
class GeogridIndex {
public:
  int tile_bdr = 3;    // border to put around each data tile
  int nx = 0;           // global image size in longitude (x)
  int ny = 0;           // global image size in latitude (y)
  int nz = 1;           // global image size vertically (z)
  int tx = 100;          // tile size in longitude (x)
  int ty = 100;          // tile size in latitude (y)
  int tz_s = 0;         // tile starting index in z (unused for 2d images)
  int tz_e = 0;         // tile ending index in z (unused for 2d images)
  bool isigned = true;  // data is signed
  bool endian = true;   // output endianness, true: little, false: big
  float scalefactor = 1.f; // amount to scale output before truncating to int
  int wordsize = 2;     // number of bytes/value in output
  Projection proj = Projection::RegularLL;
  bool categorical = false;
  std::string units = "\"NO UNITS\"";
  std::string description = "\"NO DESCRIPTION\"";
  int cat_min = 0;    // minimum category (unused for continuous data)
  int cat_max = 0;    // maximum category (unused for continuous data)
  float missing = 0.f; // value to enter for missing data
  bool bottom_top = false; // true: rows ordered bottom to top

  // Remaining fields are projection specific; see geogrid documentation.
  float dx = 0.f;
  float dy = 0.f;
  int known_x = 1;
  int known_y = 1;
  float known_lat = 0.f;
  float known_lon = 0.f;
  float stdlon = 0.f;
  float truelat1 = 0.f;
  float truelat2 = 0.f;

  // Writes geogrid metadata to `path`. Throws GeoConvertError on failure.
  void write_index_file(const std::string &path) const;
};

} // namespace convert_geotiff

#endif
