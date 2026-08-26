#include "convert_geotiff/geogrid_index.hpp"

#include <cstdio>

namespace convert_geotiff {

// printf's %f/%e formatting is used throughout (not iostream's default
// float formatting, which differs) to keep byte-identical output with the
// original C implementation.
void GeogridIndex::write_index_file(const std::string &path) const {
  FILE *f = std::fopen(path.c_str(), "w");
  if (!f) {
    throw GeoConvertError("Could not open '" + path + "' for writing.");
  }

  // Each projection takes different parameters, per module_map_utils.F.
  switch (proj) {
    case Projection::Lambert:
      std::fprintf(f, "projection = lambert\n");
      std::fprintf(f, "truelat1 = %f\n", truelat1);
      std::fprintf(f, "truelat2 = %f\n", truelat2);
      std::fprintf(f, "stdlon = %f\n", stdlon);
      std::fprintf(f, "known_x = %i\n", known_x);
      std::fprintf(f, "known_y = %i\n", known_y);
      std::fprintf(f, "known_lat = %f\n", known_lat);
      std::fprintf(f, "known_lon = %f\n", known_lon);
      std::fprintf(f, "dx = %e\n", dx);
      std::fprintf(f, "dy = %e\n", dy);
      break;
    case Projection::Polar:
      std::fprintf(f, "projection = polar\n");
      std::fprintf(f, "truelat1 = %f\n", truelat1);
      std::fprintf(f, "stdlon = %f\n", stdlon);
      std::fprintf(f, "known_x = %i\n", known_x);
      std::fprintf(f, "known_y = %i\n", known_y);
      std::fprintf(f, "known_lat = %f\n", known_lat);
      std::fprintf(f, "known_lon = %f\n", known_lon);
      std::fprintf(f, "dx = %e\n", dx);
      std::fprintf(f, "dy = %e\n", dy);
      break;
    case Projection::Mercator:
      std::fprintf(f, "projection = mercator\n");
      std::fprintf(f, "truelat1 = %f\n", truelat1);
      std::fprintf(f, "stdlon = %f\n", stdlon);
      std::fprintf(f, "known_x = %i\n", known_x);
      std::fprintf(f, "known_y = %i\n", known_y);
      std::fprintf(f, "known_lat = %f\n", known_lat);
      std::fprintf(f, "known_lon = %f\n", known_lon);
      std::fprintf(f, "dx = %e\n", dx);
      std::fprintf(f, "dy = %e\n", dy);
      break;
    case Projection::RegularLL:
      std::fprintf(f, "projection = regular_ll\n");
      std::fprintf(f, "known_x = %i\n", known_x);
      std::fprintf(f, "known_y = %i\n", known_y);
      std::fprintf(f, "known_lat = %f\n", known_lat);
      std::fprintf(f, "known_lon = %f\n", known_lon);
      std::fprintf(f, "dx = %e\n", dx);
      std::fprintf(f, "dy = %e\n", dy);
      break;
    case Projection::AlbersNad83:
      std::fprintf(f, "projection = albers_nad83\n");
      std::fprintf(f, "truelat1 = %f\n", truelat1);
      std::fprintf(f, "truelat2 = %f\n", truelat2);
      std::fprintf(f, "stdlon = %f\n", stdlon);
      std::fprintf(f, "known_x = %i\n", known_x);
      std::fprintf(f, "known_y = %i\n", known_y);
      std::fprintf(f, "known_lat = %f\n", known_lat);
      std::fprintf(f, "known_lon = %f\n", known_lon);
      std::fprintf(f, "dx = %e\n", dx);
      std::fprintf(f, "dy = %e\n", dy);
      break;
  }

  std::fprintf(f, "type = %s\n", categorical ? "categorical" : "continuous");
  std::fprintf(f, "signed = %s\n", isigned ? "yes" : "no");
  std::fprintf(f, "units = %s\n", units.c_str());
  std::fprintf(f, "description = %s\n", description.c_str());
  std::fprintf(f, "wordsize = %i\n", wordsize);
  std::fprintf(f, "tile_x = %i\n", tx);
  std::fprintf(f, "tile_y = %i\n", ty);

  if (nz > 0) {
    std::fprintf(f, "tile_z = %i\n", nz);
  } else {
    std::fprintf(f, "tile_z_start = %i\n", tz_s);
    std::fprintf(f, "tile_z_end = %i\n", tz_e);
  }

  if (categorical) {
    std::fprintf(f, "category_min = %i\n", cat_min);
    std::fprintf(f, "category_max = %i\n", cat_max);
  }

  std::fprintf(f, "tile_bdr = %i\n", tile_bdr);
  std::fprintf(f, "missing_value = %f\n", missing);
  std::fprintf(f, "scale_factor = %f\n", scalefactor);

  std::fprintf(f, "row_order = bottom_top\n");
  std::fprintf(f, "endian = %s\n", endian ? "little" : "big");

  std::fclose(f);
}

} // namespace convert_geotiff
