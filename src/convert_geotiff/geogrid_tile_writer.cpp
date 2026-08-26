#include "convert_geotiff/geogrid_tile_writer.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace convert_geotiff {

namespace {

// Matches the original write_geogrid.c's `iarray_t` (an `unsigned long`, 8
// bytes on this platform) so that any edge-case truncation behavior from
// fabs()/cast on out-of-range scaled values matches exactly.
using IArrayT = uint64_t;

// Writes one tile (including border) to disk in the geogrid binary format.
// `nx`/`ny`/`nz` are the full tile dimensions (including the border);
// `ix`/`iy` are the global 1-based starting indices of the tile's interior
// (excluding border). Throws GeoConvertError on failure.
void write_binary_tile(const std::vector<float> &rarray, int nx, int ix, int ny, int iy,
                        int nz, const GeogridIndex &idx) {
  const int ixs = ix;
  const int iys = iy;
  const int ixe = ixs + nx - 2 * idx.tile_bdr - 1;
  const int iye = iys + ny - 2 * idx.tile_bdr - 1;

  // Tile filenames are fixed 5-digit zero-padded fields ("%5.5i"); refuse to
  // silently truncate/overflow an out-of-range tile index.
  if (ixs < 0 || ixe > 99999 || iys < 0 || iye > 99999) {
    throw GeoConvertError("Tile index out of range for 5-digit filename: " +
                           std::to_string(ixs) + "-" + std::to_string(ixe) + "." +
                           std::to_string(iys) + "-" + std::to_string(iye));
  }

  // narray is computed in `long` since nx*ny*nz can exceed INT_MAX for
  // large tiles.
  const long narray = static_cast<long>(nx) * ny * nz;
  std::vector<IArrayT> iarray(static_cast<size_t>(narray));
  std::vector<unsigned char> barray(static_cast<size_t>(narray) * idx.wordsize);

  // Scale real-valued array by scalefactor and convert to integers.
  for (long i = 0; i < narray; ++i)
    iarray[i] = static_cast<IArrayT>(std::fabs(rarray[i] / idx.scalefactor));

  // Set up byte offsets for each wordsize depending on byte order. A, B, C,
  // D give the offsets of the MSB through LSB in the array from the
  // beginning of a word.
  int a2, b2, a3, b3, c3, a4, b4, c4, d4;
  if (!idx.endian) { // false = big endian
    a2 = 0; b2 = 1;
    a3 = 0; b3 = 1; c3 = 2;
    a4 = 0; b4 = 1; c4 = 2; d4 = 3;
  } else {
    b2 = 0; a2 = 1;
    c3 = 0; b3 = 1; a3 = 2;
    d4 = 0; c4 = 1; b4 = 2; a4 = 3;
  }

  switch (idx.wordsize) {
    case 1:
      for (long i = 0; i < narray; ++i) {
        if (rarray[i] < 0 && idx.isigned) iarray[i] = -iarray[i];
        barray[idx.wordsize * i] = static_cast<unsigned char>(iarray[i] & 0xff);
      }
      break;
    case 2:
      for (long i = 0; i < narray; ++i) {
        if (rarray[i] < 0 && idx.isigned) iarray[i] = -iarray[i];
        barray[idx.wordsize * i + a2] = static_cast<unsigned char>((iarray[i] >> 8) & 0xff);
        barray[idx.wordsize * i + b2] = static_cast<unsigned char>(iarray[i] & 0xff);
      }
      break;
    case 3:
      for (long i = 0; i < narray; ++i) {
        if (rarray[i] < 0 && idx.isigned) iarray[i] = -iarray[i];
        barray[idx.wordsize * i + a3] = static_cast<unsigned char>((iarray[i] >> 16) & 0xff);
        barray[idx.wordsize * i + b3] = static_cast<unsigned char>((iarray[i] >> 8) & 0xff);
        barray[idx.wordsize * i + c3] = static_cast<unsigned char>(iarray[i] & 0xff);
      }
      break;
    case 4:
      for (long i = 0; i < narray; ++i) {
        if (rarray[i] < 0 && idx.isigned) iarray[i] = -iarray[i];
        barray[idx.wordsize * i + a4] = static_cast<unsigned char>((iarray[i] >> 24) & 0xff);
        barray[idx.wordsize * i + b4] = static_cast<unsigned char>((iarray[i] >> 16) & 0xff);
        barray[idx.wordsize * i + c4] = static_cast<unsigned char>((iarray[i] >> 8) & 0xff);
        barray[idx.wordsize * i + d4] = static_cast<unsigned char>(iarray[i] & 0xff);
      }
      break;
    default:
      throw GeoConvertError("Unsupported wordsize=" + std::to_string(idx.wordsize));
  }

  // 24 bytes is enough for the actual "%5.5i-%5.5i.%5.5i-%5.5i" output
  // (ixs/ixe/iys/iye are range-checked to [0,99999] above), but GCC's
  // -Wformat-truncation only sees the declared `int` range and warns on a
  // 24-byte buffer; a generously oversized buffer avoids the false
  // positive without changing the format or its output.
  char fname[64];
  std::snprintf(fname, sizeof(fname), "%5.5i-%5.5i.%5.5i-%5.5i", ixs, ixe, iys, iye);

  FILE *bfile = std::fopen(fname, "wb");
  if (!bfile) {
    throw GeoConvertError(std::string("Could not open '") + fname + "' for writing.");
  }
  std::fwrite(barray.data(), sizeof(unsigned char), static_cast<size_t>(narray) * idx.wordsize, bfile);
  std::fclose(bfile);
}

} // namespace

void process_buffer(const GeogridIndex &idx, std::vector<float> &databuf) {
  if (!idx.categorical) return;
  for (auto &v : databuf) {
    if (static_cast<float>(static_cast<int>(v)) != v || v > idx.cat_max || v < idx.cat_min)
      v = idx.missing;
  }
}

int GeogridTileWriter::nx_tiles() const {
  return static_cast<int>(std::ceil(static_cast<double>(idx_.nx) / idx_.tx));
}

int GeogridTileWriter::ny_tiles() const {
  return static_cast<int>(std::ceil(static_cast<double>(idx_.ny) / idx_.ty));
}

int GeogridTileWriter::nz_size() const {
  if (idx_.nz <= 0) return idx_.tz_e - idx_.tz_s + 1;
  return idx_.nz;
}

long GeogridTileWriter::tile_start(int itile_x, int itile_y) const {
  const long sx = static_cast<long>(itile_x) * idx_.tx - idx_.tile_bdr;
  const long sy = static_cast<long>(itile_y) * idx_.ty - idx_.tile_bdr;
  return sy * static_cast<long>(idx_.nx) + sx;
}

long GeogridTileWriter::global_y_stride() const { return static_cast<long>(idx_.nx); }

long GeogridTileWriter::global_z_stride() const {
  return static_cast<long>(idx_.nx) * idx_.ny;
}

std::vector<float> GeogridTileWriter::extract_tile(int itile_x, int itile_y,
                                                     const std::vector<float> &databuf) const {
  const int nz = nz_size();
  const size_t tile_size = static_cast<size_t>(idx_.tx + 2 * idx_.tile_bdr) *
                            (idx_.ty + 2 * idx_.tile_bdr) * nz;
  std::vector<float> tile(tile_size);

  const long nimg = static_cast<long>(idx_.nx) * idx_.ny * nz;
  long i0 = tile_start(itile_x, itile_y);
  float *tptr = tile.data();
  for (int z = 0; z < nz; ++z) {
    long i1 = i0;
    for (int y = -idx_.tile_bdr; y < idx_.ty + idx_.tile_bdr; ++y) {
      // `read_idx` mirrors the original get_tile_from_f()'s `gptr`: it
      // walks forward one element per x iteration starting from i1,
      // *independently* of the `i1 + x` validity check below. Because i1
      // already has tile_bdr folded into its column term, this walk (not
      // `i1 + x`) is what lands on the geometrically-correct global cell;
      // the `i1 + x` check is genuinely offset by tile_bdr from it (a
      // preexisting quirk, not something introduced by this port) and is
      // kept bit-for-bit so tile output matches the original exactly,
      // including right at the corners/edges of the dataset where the two
      // formulas disagree.
      long read_idx = i1;
      for (int x = -idx_.tile_bdr; x < idx_.tx + idx_.tile_bdr; ++x) {
        if (i1 + x >= nimg || i1 + x < 0)
          *tptr++ = idx_.missing;
        else if (read_idx >= 0 && read_idx < nimg)
          *tptr++ = databuf[read_idx];
        else
          *tptr++ = idx_.missing;
        ++read_idx;
      }
      i1 += global_y_stride();
    }
    i0 += global_z_stride();
  }
  return tile;
}

void GeogridTileWriter::write_tile(int itile_x, int itile_y, const std::vector<float> &tile) const {
  const int itx = itile_x * idx_.tx + 1;
  const int ity = itile_y * idx_.ty + 1;
  const int nx = idx_.tx + 2 * idx_.tile_bdr;
  const int ny = idx_.ty + 2 * idx_.tile_bdr;
  const int nz = nz_size();

  write_binary_tile(tile, nx, itx, ny, ity, nz, idx_);
}

} // namespace convert_geotiff
