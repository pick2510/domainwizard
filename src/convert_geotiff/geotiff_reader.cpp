#include "convert_geotiff/geotiff_reader.hpp"

#include <geo_normalize.h>
#include <geo_tiffp.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace convert_geotiff {

namespace {

bool is_bigendian() {
  const int test_val = 1;
  return *reinterpret_cast<const char *>(&test_val) == 0;
}

// RAII wrapper for a GTIF* handle so early-throw paths in get_index() can't
// leak it.
class GtifHandle {
public:
  explicit GtifHandle(TIFF *tif) : gtif_(GTIFNew(tif)) {}
  ~GtifHandle() {
    if (gtif_) GTIFFree(gtif_);
  }
  GtifHandle(const GtifHandle &) = delete;
  GtifHandle &operator=(const GtifHandle &) = delete;
  GTIF *get() const { return gtif_; }

private:
  GTIF *gtif_;
};

// Converts a raw byte buffer of DType samples into a float buffer.
template <typename DType>
std::vector<float> convert_buffer(const std::vector<unsigned char> &raw, size_t count) {
  std::vector<float> out(count);
  for (size_t i = 0; i < count; ++i) {
    DType value;
    std::memcpy(&value, raw.data() + i * sizeof(DType), sizeof(DType));
    out[i] = static_cast<float>(value);
  }
  return out;
}

// Converts a raw byte buffer of DType samples, pixel-interleaved across
// `nz` bands (PLANARCONFIG_CONTIG: all bands of pixel 0, then all bands of
// pixel 1, ...), into a z-major float buffer (each band occupies a
// contiguous nx*ny plane) -- matching GeogridTileWriter's z-stride
// convention, and the exact inverse of geotiff_writer.cpp's own
// band-interleaving when it writes a multi-level dataset back out.
template <typename DType>
std::vector<float> deinterleave_buffer(const std::vector<unsigned char> &raw, size_t plane_count, size_t nz) {
  std::vector<float> out(plane_count * nz);
  for (size_t i = 0; i < plane_count; ++i) {
    for (size_t z = 0; z < nz; ++z) {
      DType value;
      std::memcpy(&value, raw.data() + (i * nz + z) * sizeof(DType), sizeof(DType));
      out[z * plane_count + i] = static_cast<float>(value);
    }
  }
  return out;
}

} // namespace

GeoTiffFile::GeoTiffFile(const std::string &path) {
  TIFFSetWarningHandler(nullptr);
  tif_ = XTIFFOpen(path.c_str(), "r");
  if (!tif_) {
    throw GeoConvertError("Could not open file " + path + ".");
  }
}

GeoTiffFile::~GeoTiffFile() {
  if (tif_) XTIFFClose(tif_);
}

GeogridIndex GeoTiffFile::get_index() const {
  GeogridIndex idx;

  GtifHandle gtifh(tif_);
  GTIFDefn gtifp;
  GTIFGetDefn(gtifh.get(), &gtifp);

  // GTIFKeyGet does not guarantee its output is set on failure, so every
  // local read this way is pre-initialized and a failed lookup is reported
  // instead of silently propagating whatever value was left.
  double stdpar1 = 0., stdpar2 = 0., stdlon = 0., olat = 0., olon = 0.;

  if (!GTIFKeyGet(gtifh.get(), ProjStdParallel1GeoKey, &stdpar1, 0, 1))
    std::fprintf(stderr, "WARNING: could not read ProjStdParallel1GeoKey, using 0.\n");
  idx.truelat1 = static_cast<float>(stdpar1);

  if (!GTIFKeyGet(gtifh.get(), ProjStdParallel2GeoKey, &stdpar2, 0, 1))
    std::fprintf(stderr, "WARNING: could not read ProjStdParallel2GeoKey, using 0.\n");
  idx.truelat2 = static_cast<float>(stdpar2);

  if (!GTIFKeyGet(gtifh.get(), ProjCenterLongGeoKey, &stdlon, 0, 1))
    std::fprintf(stderr, "WARNING: could not read ProjCenterLongGeoKey, using 0.\n");
  idx.stdlon = static_cast<float>(stdlon);

  // The pixel-scale tag holds a variable-length array; libtiff's convention
  // for such tags is that the caller passes the address of a pointer
  // variable, which libtiff then points at its own internal buffer. This
  // local is deliberately a plain pointer (never a fixed-size array) so
  // TIFFGetField can never write through a mismatched type.
  double *pixelscale = nullptr;
  int count = 0;
  if (TIFFGetField(tif_, GTIFF_PIXELSCALE, &count, &pixelscale) && count >= 2) {
    idx.dx = static_cast<float>(pixelscale[0]);
    idx.dy = static_cast<float>(pixelscale[1]);
  } else {
    idx.dx = 0.f;
    idx.dy = 0.f;
  }

  // Fill projection specific parameters.
  // WARNING: This is far from robust and will likely break for certain
  // GeoTIFF files.
  short modeltype = 0;
  GTIFKeyGet(gtifh.get(), GTModelTypeGeoKey, &modeltype, 0, 1);
  int projid = gtifp.CTProjection;
  switch (projid) {
    case CT_AlbersEqualArea:
      idx.proj = Projection::AlbersNad83;
      if (!GTIFKeyGet(gtifh.get(), ProjNatOriginLatGeoKey, &olat, 0, 1) ||
          !GTIFKeyGet(gtifh.get(), ProjNatOriginLongGeoKey, &olon, 0, 1))
        std::fprintf(stderr, "WARNING: could not read Albers origin lat/lon, using 0.\n");
      idx.known_lat = static_cast<float>(olat);
      idx.known_lon = static_cast<float>(olon);
      break;
    case CT_TransverseMercator:
      idx.proj = Projection::Mercator;
      if (!GTIFKeyGet(gtifh.get(), ProjNatOriginLatGeoKey, &olat, 0, 1) ||
          !GTIFKeyGet(gtifh.get(), ProjNatOriginLongGeoKey, &olon, 0, 1))
        std::fprintf(stderr, "WARNING: could not read Mercator origin lat/lon, using 0.\n");
      idx.known_lat = static_cast<float>(olat);
      idx.known_lon = static_cast<float>(olon);
      break;
    case CT_PolarStereographic:
      idx.proj = Projection::Polar;
      if (!GTIFKeyGet(gtifh.get(), ProjNatOriginLatGeoKey, &olat, 0, 1) ||
          !GTIFKeyGet(gtifh.get(), ProjNatOriginLongGeoKey, &olon, 0, 1))
        std::fprintf(stderr, "WARNING: could not read Polar Stereographic origin lat/lon, using 0.\n");
      idx.known_lat = static_cast<float>(olat);
      idx.known_lon = static_cast<float>(olon);
      break;
    case CT_LambertConfConic:
      idx.proj = Projection::Lambert;
      if (!GTIFKeyGet(gtifh.get(), ProjFalseOriginLatGeoKey, &olat, 0, 1) ||
          !GTIFKeyGet(gtifh.get(), ProjFalseOriginLongGeoKey, &olon, 0, 1))
        std::fprintf(stderr, "WARNING: could not read Lambert false-origin lat/lon, using 0.\n");
      idx.known_lat = static_cast<float>(olat);
      idx.known_lon = static_cast<float>(olon);
      break;
    default:
      if (modeltype == ModelTypeGeographic) {
        idx.proj = Projection::RegularLL;
      } else {
        throw GeoConvertError("Unknown projection ID: " + std::to_string(projid));
      }
  }

  // Get coordinates of lower left corner.
  olon = 0;
  olat = 0;
  idx.known_x = 1;
  idx.known_y = 1;
  if (modeltype == ModelTypeGeographic) {
    if (!GTIFImageToPCS(gtifh.get(), &olon, &olat)) {
      std::fprintf(stderr, "WARNING: cannot get coordinates of lower left corner.\n");
      std::fprintf(stderr, "You will have to edit the index file manually.\n");
    }
    idx.known_lat = static_cast<float>(olat);
    idx.known_lon = static_cast<float>(olon);

    olat = 1;
    olon = 1;
    GTIFImageToPCS(gtifh.get(), &olon, &olat);
    if (idx.dx <= 0.f && idx.dy <= 0.f) {
      // Last resort: no ModelPixelScaleTag, so estimate dx/dy from the
      // coordinate delta of a single diagonal (1,1) pixel step. This is
      // only correct if the image-to-PCS transform has no rotation/shear
      // -- warn loudly since that assumption is not otherwise checked.
      std::fprintf(stderr,
                    "WARNING: no valid pixel-scale tag found; estimating "
                    "dx/dy from a single diagonal pixel step instead. This "
                    "is only correct if the raster has no rotation/shear -- "
                    "verify the resulting dx/dy in the index file by hand.\n");
      idx.dx = static_cast<float>(std::fabs(olon - static_cast<double>(idx.known_lon)));
      idx.dy = static_cast<float>(std::fabs(olat - static_cast<double>(idx.known_lat)));
    }
  } else {
    if (!GTIFImageToPCS(gtifh.get(), &olon, &olat)) {
      std::fprintf(stderr, "WARNING: cannot get coordinates of lower left corner.\n");
      std::fprintf(stderr, "You will have to edit the index file manually.\n");
    }
    if (!GTIFProj4ToLatLong(&gtifp, 1, &olon, &olat)) {
      std::fprintf(stderr, "WARNING: cannot convert from PCS to lat/lon.\n");
    }
    idx.known_lat = static_cast<float>(olat);
    idx.known_lon = static_cast<float>(olon);

    olat = 1;
    olon = 1;
    GTIFImageToPCS(gtifh.get(), &olon, &olat);
    GTIFProj4ToLatLong(&gtifp, 1, &olon, &olat);
    if (idx.dx <= 0.f && idx.dy <= 0.f) {
      // Last resort: same caveat as above -- only correct absent
      // rotation/shear.
      std::fprintf(stderr,
                    "WARNING: no valid pixel-scale tag found; estimating "
                    "dx/dy from a single diagonal pixel step instead. This "
                    "is only correct if the raster has no rotation/shear -- "
                    "verify the resulting dx/dy in the index file by hand.\n");
      idx.dx = static_cast<float>(std::fabs(olon - static_cast<double>(idx.known_lon)));
      idx.dy = static_cast<float>(std::fabs(olat - static_cast<double>(idx.known_lat)));
    }
  }

  // Fill parameters from TIFF i/o.
  uint32_t inx = 0, iny = 0, inz = 0;
  if (!TIFFGetField(tif_, TIFFTAG_IMAGEWIDTH, &inx) ||
      !TIFFGetField(tif_, TIFFTAG_IMAGELENGTH, &iny)) {
    throw GeoConvertError("Could not find image dimensions in open file.");
  }
  idx.nx = static_cast<int>(inx);
  idx.ny = static_cast<int>(iny);

  // nz comes from one of two mutually-exclusive sources: the rarely-used
  // volumetric TIFFTAG_IMAGEDEPTH extension, or -- far more common in
  // practice -- a standard multi-band GeoTIFF (samples per pixel > 1),
  // written as tile_z levels one band each. See read_buffer() for how
  // each is actually read.
  TIFFGetField(tif_, TIFFTAG_IMAGEDEPTH, &inz);
  if (inz == 0) inz = 1;

  uint16_t samples_per_pixel = 0;
  TIFFGetField(tif_, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
  if (samples_per_pixel == 0) samples_per_pixel = 1;

  if (inz > 1 && samples_per_pixel > 1) {
    throw GeoConvertError(
        "File has both TIFFTAG_IMAGEDEPTH > 1 and multiple samples per pixel; "
        "ambiguous source for tile_z.");
  }
  idx.nz = (inz > 1) ? static_cast<int>(inz) : static_cast<int>(samples_per_pixel);
  idx.tz_s = 0;
  idx.tz_e = idx.tz_s + idx.nz - 1;

  // Get orientation of the data, defaults to TOPLEFT.
  uint16_t orientation = ORIENTATION_TOPLEFT;
  TIFFGetField(tif_, TIFFTAG_ORIENTATION, &orientation);

  switch (orientation) {
    case ORIENTATION_TOPLEFT:
      idx.bottom_top = false;
      // If TOPLEFT orientation, pixel (0,0) is actually the top-left
      // pixel, so adjust known_y here.
      idx.known_y = idx.ny;
      break;
    case ORIENTATION_BOTLEFT:
      idx.bottom_top = true;
      break;
    default:
      throw GeoConvertError("Unsupported image orientation.");
  }

  // Get the data type of the pixels, only supporting b/w images.
  uint16_t format = SAMPLEFORMAT_UINT;
  TIFFGetField(tif_, TIFFTAG_SAMPLEFORMAT, &format);

  switch (format) {
    case SAMPLEFORMAT_UINT:
      idx.isigned = false;
      break;
    case SAMPLEFORMAT_INT:
      idx.isigned = true;
      break;
    case SAMPLEFORMAT_IEEEFP:
      idx.isigned = true;
      break;
    default:
      throw GeoConvertError("Unsupported pixel format.");
  }

  // libtiff always returns the buffer in native machine endian.
  idx.endian = !is_bigendian();

  return idx;
}

std::vector<float> GeoTiffFile::read_buffer() const {
  uint32_t inx = 0, iny = 0, inz = 1;
  if (!TIFFGetField(tif_, TIFFTAG_IMAGEWIDTH, &inx) ||
      !TIFFGetField(tif_, TIFFTAG_IMAGELENGTH, &iny)) {
    throw GeoConvertError("Could not find image dimensions in open file.");
  }
  TIFFGetField(tif_, TIFFTAG_IMAGEDEPTH, &inz);
  if (inz == 0) inz = 1;

  uint16_t bits_per_sample = 0;
  if (!TIFFGetField(tif_, TIFFTAG_BITSPERSAMPLE, &bits_per_sample)) {
    throw GeoConvertError("Could not find TIFFTAG_BITSPERSAMPLE.");
  }

  // Only 1, 2, and 4 byte samples are supported.
  int bytes_per_sample;
  switch (bits_per_sample) {
    case 8: bytes_per_sample = 1; break;
    case 16: bytes_per_sample = 2; break;
    case 32: bytes_per_sample = 4; break;
    default:
      throw GeoConvertError("Unsupported bits_per_sample=" + std::to_string(bits_per_sample) + ".");
  }

  // A single channel (b/w) image is the common case; a multi-band image
  // is read as tile_z levels, one band each (get_index() already
  // validates this isn't combined with TIFFTAG_IMAGEDEPTH). Only
  // PLANARCONFIG_CONTIG (the default, and what geotiff_writer.cpp itself
  // produces) is supported for multi-band input.
  uint16_t samples_per_pixel = 0;
  if (!TIFFGetField(tif_, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel)) {
    throw GeoConvertError("Could not find TIFFTAG_SAMPLESPERPIXEL.");
  }
  if (samples_per_pixel == 0) samples_per_pixel = 1;
  if (samples_per_pixel > 1) {
    uint16_t planar_config = PLANARCONFIG_CONTIG;
    TIFFGetField(tif_, TIFFTAG_PLANARCONFIG, &planar_config);
    if (planar_config != PLANARCONFIG_CONTIG) {
      throw GeoConvertError("Multi-band images must use PLANARCONFIG_CONTIG (band-interleaved-by-pixel).");
    }
  }
  const bool multiband = samples_per_pixel > 1;

  uint16_t sample_format = SAMPLEFORMAT_UINT;
  TIFFGetField(tif_, TIFFTAG_SAMPLEFORMAT, &sample_format);

  // "Pixel" here (and pixel_idx/pixel_count below) means one full
  // multi-band pixel, i.e. inx*iny*inz positions -- not one sample.
  // pixel_count is the total number of *samples* (pixels * bands), used
  // for buffer sizing and for the flat-conversion paths below.
  const size_t pixel_plane_count = static_cast<size_t>(inx) * iny * inz;
  const size_t pixel_count = pixel_plane_count * samples_per_pixel;
  const size_t buffer_size = pixel_count * static_cast<size_t>(bytes_per_sample);
  std::vector<unsigned char> buffer(buffer_size);

  if (TIFFIsTiled(tif_)) {
    uint32_t tile_width = 0, tile_length = 0;
    TIFFGetField(tif_, TIFFTAG_TILEWIDTH, &tile_width);
    TIFFGetField(tif_, TIFFTAG_TILELENGTH, &tile_length);

    std::vector<unsigned char> tile_buf(TIFFTileSize(tif_));

    for (uint32_t k = 0; k < inz; ++k) {
      for (uint32_t j = 0; j < iny; j += tile_length) {
        for (uint32_t i = 0; i < inx; i += tile_width) {
          if (TIFFReadTile(tif_, tile_buf.data(), i, j, k, 0) == static_cast<tsize_t>(-1)) {
            throw GeoConvertError("Read error on input tile number " + std::to_string(i) +
                                   "," + std::to_string(j));
          }

          const unsigned char *tptr = tile_buf.data();
          const size_t pixel_stride = static_cast<size_t>(samples_per_pixel) * bytes_per_sample;
          for (uint32_t j0 = 0; j0 < tile_length; ++j0) {
            uint32_t j1 = j0 + j;
            uint32_t i1 = i;

            const size_t pixel_idx = static_cast<size_t>(k) * inx * iny + static_cast<size_t>(j1) * inx + i1;
            if (pixel_idx < pixel_plane_count) {
              unsigned char *bptr = buffer.data() + pixel_idx * pixel_stride;
              std::memcpy(bptr, tptr, static_cast<size_t>(tile_width) * pixel_stride);
            }
            tptr += static_cast<size_t>(tile_width) * pixel_stride;
          }
        }
      }
    }
  } else {
    // Read in the possibly multiple strips. This is easier than tiles
    // because we don't have to worry about strides.
    tsize_t strip_size = TIFFStripSize(tif_);
    tstrip_t strip_max = TIFFNumberOfStrips(tif_);
    size_t image_offset = 0;
    for (tstrip_t strip = 0; strip < strip_max; ++strip) {
      tsize_t result = TIFFReadEncodedStrip(tif_, strip, buffer.data() + image_offset, strip_size);
      if (result == static_cast<tsize_t>(-1)) {
        throw GeoConvertError("Read error on input strip number " + std::to_string(strip));
      }
      image_offset += static_cast<size_t>(result);
    }
  }

  // Convert image buffer into float. Multi-band data is de-interleaved
  // into a z-major buffer (see deinterleave_buffer()); everything else
  // (the common single-band case, and the volumetric IMAGEDEPTH case,
  // which is already read z-plane by z-plane above) is a flat conversion.
  switch (sample_format) {
    case SAMPLEFORMAT_UINT:
      switch (bytes_per_sample) {
        case 1: return multiband ? deinterleave_buffer<uint8_t>(buffer, pixel_plane_count, samples_per_pixel)
                                  : convert_buffer<uint8_t>(buffer, pixel_count);
        case 2: return multiband ? deinterleave_buffer<uint16_t>(buffer, pixel_plane_count, samples_per_pixel)
                                  : convert_buffer<uint16_t>(buffer, pixel_count);
        case 4: return multiband ? deinterleave_buffer<uint32_t>(buffer, pixel_plane_count, samples_per_pixel)
                                  : convert_buffer<uint32_t>(buffer, pixel_count);
        default:
          throw GeoConvertError("Unsupported bytes per sample=" + std::to_string(bytes_per_sample) + " for uint.");
      }
    case SAMPLEFORMAT_INT:
      switch (bytes_per_sample) {
        case 1: return multiband ? deinterleave_buffer<int8_t>(buffer, pixel_plane_count, samples_per_pixel)
                                  : convert_buffer<int8_t>(buffer, pixel_count);
        case 2: return multiband ? deinterleave_buffer<int16_t>(buffer, pixel_plane_count, samples_per_pixel)
                                  : convert_buffer<int16_t>(buffer, pixel_count);
        case 4: return multiband ? deinterleave_buffer<int32_t>(buffer, pixel_plane_count, samples_per_pixel)
                                  : convert_buffer<int32_t>(buffer, pixel_count);
        default:
          throw GeoConvertError("Unsupported bytes per sample=" + std::to_string(bytes_per_sample) + " for int.");
      }
    case SAMPLEFORMAT_IEEEFP:
      switch (bytes_per_sample) {
        case sizeof(float): {
          if (multiband) return deinterleave_buffer<float>(buffer, pixel_plane_count, samples_per_pixel);
          std::vector<float> out(pixel_count);
          std::memcpy(out.data(), buffer.data(), pixel_count * sizeof(float));
          return out;
        }
        case sizeof(double): return multiband
                                        ? deinterleave_buffer<double>(buffer, pixel_plane_count, samples_per_pixel)
                                        : convert_buffer<double>(buffer, pixel_count);
        default:
          throw GeoConvertError("Unsupported bytes per sample=" + std::to_string(bytes_per_sample) + " for IEEEFP.");
      }
    default:
      throw GeoConvertError("Unsupported data type in image.");
  }
}

} // namespace convert_geotiff
