#include "convert_geotiff/geotiff_writer.hpp"

#include <geo_normalize.h>
#include <geo_tiffp.h>
#include <geotiffio.h>
#include <xtiffio.h>
#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace convert_geotiff {

namespace {

// WRF/WPS geogrid data is generated (and consumed) assuming a spherical
// earth of this radius for every projection except albers_nad83 -- see
// the "Earth model" section of the reverse-conversion plan. Writing this
// back as a user-defined GeoTIFF ellipsoid (semi-major = radius,
// inv-flattening = 0, GeoTIFF's way of saying "sphere") is the honest
// reverse of what geotiff_reader.cpp assumes on the way in.
constexpr double kSphereRadiusMeters = 6370000.0;

// Writes the geographic (datum/ellipsoid) GeoKeys shared by every
// projection -- the base CS a projected CS sits on top of, or the CS
// itself for regular_ll.
void set_geographic_keys(GTIF *gtif, const GeogridIndex &idx) {
  if (idx.proj == Projection::AlbersNad83) {
    GTIFKeySet(gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, GCS_NAD83);
    return;
  }
  GTIFKeySet(gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
  GTIFKeySet(gtif, GeogGeodeticDatumGeoKey, TYPE_SHORT, 1, KvUserDefined);
  GTIFKeySet(gtif, GeogEllipsoidGeoKey, TYPE_SHORT, 1, KvUserDefined);
  GTIFKeySet(gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1, kSphereRadiusMeters);
  GTIFKeySet(gtif, GeogInvFlatteningGeoKey, TYPE_DOUBLE, 1, 0.0);
  GTIFKeySet(gtif, GeogAngularUnitsGeoKey, TYPE_SHORT, 1, Angular_Degree);
}

// Writes the full set of GeoKeys for `idx.proj`. Returns the CT_* transform
// code used for projected types, or -1 for regular_ll.
int set_projection_keys(GTIF *gtif, const GeogridIndex &idx) {
  if (idx.proj == Projection::RegularLL) {
    GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeGeographic);
    GTIFKeySet(gtif, GTRasterTypeGeoKey, TYPE_SHORT, 1, RasterPixelIsArea);
    set_geographic_keys(gtif, idx);
    return -1;
  }

  GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
  GTIFKeySet(gtif, GTRasterTypeGeoKey, TYPE_SHORT, 1, RasterPixelIsArea);
  GTIFKeySet(gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
  set_geographic_keys(gtif, idx);
  GTIFKeySet(gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, Linear_Meter);

  // Always set both -- geotiff_reader.cpp reads ProjStdParallel1/2GeoKey
  // and ProjCenterLongGeoKey unconditionally for every projection, before
  // it even knows which one it is.
  GTIFKeySet(gtif, ProjStdParallel1GeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.truelat1));
  GTIFKeySet(gtif, ProjStdParallel2GeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.truelat2));
  GTIFKeySet(gtif, ProjCenterLongGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.stdlon));

  // The origin latitude is not recorded in the geogrid index file (see the
  // plan's "Origin latitude is a free gauge choice" note) -- truelat1 with
  // zero false easting/northing is used as a fixed, self-consistent
  // default; the tie point is computed from this same definition below, so
  // this choice does not affect where any pixel actually ends up on the
  // ground.
  int ct;
  switch (idx.proj) {
    case Projection::Lambert:
      ct = CT_LambertConfConic;
      GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1, ct);
      GTIFKeySet(gtif, ProjFalseOriginLatGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.truelat1));
      GTIFKeySet(gtif, ProjFalseOriginLongGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.stdlon));
      GTIFKeySet(gtif, ProjFalseOriginEastingGeoKey, TYPE_DOUBLE, 1, 0.0);
      GTIFKeySet(gtif, ProjFalseOriginNorthingGeoKey, TYPE_DOUBLE, 1, 0.0);
      break;
    case Projection::Polar:
      ct = CT_PolarStereographic;
      GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1, ct);
      GTIFKeySet(gtif, ProjNatOriginLatGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.truelat1));
      GTIFKeySet(gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.stdlon));
      GTIFKeySet(gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 0.0);
      GTIFKeySet(gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1, 0.0);
      break;
    case Projection::Mercator:
      // Written back as CT_TransverseMercator, not CT_Mercator: this is
      // the tag geotiff_reader.cpp's forward reader maps to
      // Projection::Mercator (it doesn't handle plain CT_Mercator at all)
      // -- preserved here for GeoTIFF -> geogrid -> GeoTIFF round-trip
      // stability, not because it's the "correct" tag for a true Mercator
      // projection.
      ct = CT_TransverseMercator;
      GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1, ct);
      GTIFKeySet(gtif, ProjNatOriginLatGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.truelat1));
      GTIFKeySet(gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.stdlon));
      GTIFKeySet(gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 0.0);
      GTIFKeySet(gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1, 0.0);
      break;
    case Projection::AlbersNad83:
      ct = CT_AlbersEqualArea;
      GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1, ct);
      GTIFKeySet(gtif, ProjNatOriginLatGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.truelat1));
      GTIFKeySet(gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1, static_cast<double>(idx.stdlon));
      GTIFKeySet(gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 0.0);
      GTIFKeySet(gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1, 0.0);
      break;
    default:
      ct = -1; // unreachable: RegularLL handled above
  }
  return ct;
}

} // namespace

void write_geotiff(const std::string &path, const GeogridIndex &idx, const std::vector<float> &buffer) {
  const int nz = (idx.nz > 0) ? idx.nz : (idx.tz_e - idx.tz_s + 1);
  if (nz < 1) {
    throw GeoConvertError("write_geotiff: invalid tile_z.");
  }
  if (buffer.size() != static_cast<size_t>(idx.nx) * idx.ny * nz) {
    throw GeoConvertError("write_geotiff: buffer size does not match nx*ny*nz.");
  }

  TIFFSetWarningHandler(nullptr);
  TIFF *tif = XTIFFOpen(path.c_str(), "w");
  if (!tif) {
    throw GeoConvertError("Could not open '" + path + "' for writing.");
  }

  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(idx.nx));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(idx.ny));
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
  // Multi-level (tile_z > 1) geogrid data (e.g. monthly climatology
  // fields) is written as a standard multi-band GeoTIFF, one band per
  // z-level -- broadly readable by GDAL/QGIS/etc., unlike the volumetric
  // (TIFFTAG_IMAGEDEPTH) TIFF convention geotiff_reader.cpp's forward
  // reader nominally supports but which mainstream GIS tooling barely
  // reads at all.
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(nz));
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  // DEFLATE (zlib): lossless, widely supported by GDAL/QGIS/etc. The
  // floating-point predictor meaningfully improves the compression ratio
  // for smoothly-varying continuous data (it differences the mantissa
  // bytes of neighboring samples), but categorical data is a sequence of
  // essentially arbitrary small integer codes with no such neighbor
  // correlation -- differencing them doesn't help, and can occasionally
  // make output larger than no predictor at all. Only enable it for
  // continuous data.
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
  TIFFSetField(tif, TIFFTAG_PREDICTOR, idx.categorical ? PREDICTOR_NONE : PREDICTOR_FLOATINGPOINT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));

  GTIF *gtif = GTIFNew(tif);
  if (!gtif) {
    XTIFFClose(tif);
    throw GeoConvertError("Could not create GeoTIFF key writer for '" + path + "'.");
  }

  const int ct = set_projection_keys(gtif, idx);
  GTIFWriteKeys(gtif);

  // Re-read the definition we just wrote to convert the tie point's
  // lat/lon into this projection's (easting, northing) -- the exact
  // inverse of geotiff_reader.cpp's GTIFProj4ToLatLong call. Because the
  // conversion uses the same definition we just wrote, the arbitrary
  // origin latitude chosen above cancels out.
  double tie_x = static_cast<double>(idx.known_lon);
  double tie_y = static_cast<double>(idx.known_lat);
  if (ct != -1) {
    GTIF *gtif_check = GTIFNew(tif);
    GTIFDefn defn;
    GTIFGetDefn(gtif_check, &defn);
    if (!GTIFProj4FromLatLong(&defn, 1, &tie_x, &tie_y)) {
      GTIFFree(gtif_check);
      GTIFFree(gtif);
      XTIFFClose(tif);
      throw GeoConvertError("Could not project the tie point for '" + path + "'.");
    }
    GTIFFree(gtif_check);
  }
  GTIFFree(gtif);

  // ModelPixelScaleTag must be positive per the GeoTIFF spec -- direction
  // is implied by the tiepoint/raster-space convention (row 0 = north for
  // the ORIENTATION_TOPLEFT output written below), not by the sign of the
  // scale itself. This tool's own writer always emits positive dx/dy, but
  // some real-world WPS_GEOG index files use a signed dy (e.g. -0.05) as
  // an idiosyncratic authoring convention; take the magnitude here rather
  // than propagating a spec-violating negative scale (which real readers,
  // e.g. GDAL, warn about and silently reinterpret anyway).
  double pixel_scale[3] = {std::fabs(static_cast<double>(idx.dx)), std::fabs(static_cast<double>(idx.dy)),
                            0.0};
  TIFFSetField(tif, GTIFF_PIXELSCALE, 3, pixel_scale);

  // The tile data's row 1 (as it literally appears in tile filenames, and
  // as read_tiles() copies it 1:1 into buffer row 0) is the southernmost
  // row when idx.bottom_top is set (geogrid.exe's own default, and what
  // this tool's own writer always emits - see convert.cpp's row flip to
  // bottom_top before tiling), or the northernmost row when a real-world
  // index file explicitly sets `row_order = top_bottom` - see
  // read_index_file(). Only flip buffer rows in the bottom_top case, to
  // reach ORIENTATION_TOPLEFT's row-0-is-north; top_bottom data is already
  // north-first.
  const bool flip_rows = idx.bottom_top;

  // known_x/known_y are 1-based tile-filename row/column indices (not
  // necessarily "from the south" -- see above); convert to the 0-based
  // TOPLEFT raster row that ends up holding tile-file row `known_y` after
  // the same flip-or-not decision.
  const int tie_row = flip_rows ? (idx.ny - idx.known_y) : (idx.known_y - 1);
  double tiepoint[6] = {
      static_cast<double>(idx.known_x - 1),
      static_cast<double>(tie_row),
      0.0,
      tie_x,
      tie_y,
      0.0,
  };
  TIFFSetField(tif, GTIFF_TIEPOINTS, 6, tiepoint);

  // z-major buffer (each z-level occupies a contiguous nx*ny block,
  // matching GeogridTileWriter's own z-stride convention); interleave
  // z-levels into per-pixel samples for PLANARCONFIG_CONTIG.
  std::vector<float> row(static_cast<size_t>(idx.nx) * nz);
  const size_t z_stride = static_cast<size_t>(idx.nx) * idx.ny;
  for (int y = 0; y < idx.ny; ++y) {
    const int src_row = flip_rows ? (idx.ny - 1 - y) : y;
    for (int x = 0; x < idx.nx; ++x) {
      for (int z = 0; z < nz; ++z) {
        row[static_cast<size_t>(x) * nz + z] =
            buffer[static_cast<size_t>(z) * z_stride + static_cast<size_t>(src_row) * idx.nx + x];
      }
    }
    if (TIFFWriteScanline(tif, row.data(), static_cast<uint32_t>(y), 0) < 0) {
      XTIFFClose(tif);
      throw GeoConvertError("Error writing row " + std::to_string(y) + " to '" + path + "'.");
    }
  }

  XTIFFClose(tif);
}

} // namespace convert_geotiff
