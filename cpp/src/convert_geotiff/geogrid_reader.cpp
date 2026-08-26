#include "convert_geotiff/geogrid_reader.hpp"

#include <dirent.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>

namespace convert_geotiff {

namespace {

std::string trim(const std::string &s) {
  const auto start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

// GeogridIndex::write_index_file() always writes "key = value" (spaces on
// both sides of '='), but real-world WPS_GEOG index files are inconsistent
// about this -- e.g. "projection=regular_ll" with no spaces at all, right
// next to "signed = yes" with spaces, in the same file. Split on the first
// '=' and trim whitespace from both sides instead of requiring the exact
// " = " this tool's own writer happens to produce.
std::map<std::string, std::string> parse_key_value_file(const std::string &path) {
  std::ifstream f(path);
  if (!f) {
    throw GeoConvertError("Could not open '" + path + "' for reading.");
  }
  std::map<std::string, std::string> kv;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto sep = line.find('=');
    if (sep == std::string::npos) continue;
    kv[trim(line.substr(0, sep))] = trim(line.substr(sep + 1));
  }
  return kv;
}

std::string require(const std::map<std::string, std::string> &kv, const std::string &key) {
  auto it = kv.find(key);
  if (it == kv.end()) {
    throw GeoConvertError("index file is missing required key '" + key + "'.");
  }
  return it->second;
}

std::string get_or(const std::map<std::string, std::string> &kv, const std::string &key,
                    const std::string &fallback) {
  auto it = kv.find(key);
  return it == kv.end() ? fallback : it->second;
}

float parse_float(const std::string &s) { return std::strtof(s.c_str(), nullptr); }
int parse_int(const std::string &s) { return std::atoi(s.c_str()); }

} // namespace

GeogridIndex read_index_file(const std::string &dir) {
  const std::map<std::string, std::string> kv = parse_key_value_file(dir + "/index");

  GeogridIndex idx;

  const std::string &proj = require(kv, "projection");
  if (proj == "lambert") {
    idx.proj = Projection::Lambert;
  } else if (proj == "polar") {
    idx.proj = Projection::Polar;
  } else if (proj == "mercator") {
    idx.proj = Projection::Mercator;
  } else if (proj == "regular_ll") {
    idx.proj = Projection::RegularLL;
  } else if (proj == "albers_nad83") {
    idx.proj = Projection::AlbersNad83;
  } else {
    throw GeoConvertError("index file has unknown projection '" + proj + "'.");
  }

  // truelat2 (polar/mercator) and stdlon (regular_ll) are not written for
  // every projection -- default to 0 when absent, matching those
  // projections' formulas not using them.
  idx.truelat1 = parse_float(get_or(kv, "truelat1", "0"));
  idx.truelat2 = parse_float(get_or(kv, "truelat2", "0"));
  idx.stdlon = parse_float(get_or(kv, "stdlon", "0"));
  idx.known_x = parse_int(require(kv, "known_x"));
  idx.known_y = parse_int(require(kv, "known_y"));
  idx.known_lat = parse_float(require(kv, "known_lat"));
  idx.known_lon = parse_float(require(kv, "known_lon"));
  idx.dx = parse_float(require(kv, "dx"));
  idx.dy = parse_float(require(kv, "dy"));

  idx.categorical = require(kv, "type") == "categorical";
  if (kv.count("signed")) {
    idx.isigned = kv.at("signed") == "yes";
  } else {
    // Some real-world WPS_GEOG datasets omit this key -- typically
    // byte-range categorical/fractional data (e.g. a missing_value of 255,
    // the max unsigned byte, is a common tell) where unsigned is the
    // natural interpretation; default to unsigned rather than erroring.
    std::fprintf(stderr, "WARNING: index file has no 'signed' key; assuming unsigned.\n");
    idx.isigned = false;
  }
  idx.units = require(kv, "units");
  idx.description = require(kv, "description");
  idx.wordsize = parse_int(require(kv, "wordsize"));
  idx.tx = parse_int(require(kv, "tile_x"));
  idx.ty = parse_int(require(kv, "tile_y"));

  if (kv.count("tile_z")) {
    idx.nz = parse_int(kv.at("tile_z"));
    idx.tz_s = 0;
    idx.tz_e = idx.nz - 1;
  } else {
    idx.tz_s = parse_int(require(kv, "tile_z_start"));
    idx.tz_e = parse_int(require(kv, "tile_z_end"));
    idx.nz = 0; // sentinel: GeogridTileWriter::nz_size() derives it from tz_s/tz_e
  }

  if (idx.categorical) {
    idx.cat_min = parse_int(require(kv, "category_min"));
    idx.cat_max = parse_int(require(kv, "category_max"));
  }

  // tile_bdr/missing_value/scale_factor are sometimes omitted in
  // real-world WPS_GEOG index files (observed e.g. in NCAR's own
  // topo_gmted2010_5m and soiltype_top_5m datasets); default to the same
  // values convert_geotiff's own CLI defaults to (-b 3, -m 0, -s 1) rather
  // than erroring on a field that just wasn't written.
  if (!kv.count("tile_bdr"))
    std::fprintf(stderr, "WARNING: index file has no 'tile_bdr' key; assuming 3.\n");
  idx.tile_bdr = parse_int(get_or(kv, "tile_bdr", "3"));
  if (!kv.count("missing_value"))
    std::fprintf(stderr, "WARNING: index file has no 'missing_value' key; assuming 0.\n");
  idx.missing = parse_float(get_or(kv, "missing_value", "0"));
  if (!kv.count("scale_factor"))
    std::fprintf(stderr, "WARNING: index file has no 'scale_factor' key; assuming 1.\n");
  idx.scalefactor = parse_float(get_or(kv, "scale_factor", "1"));

  idx.bottom_top = true; // write_index_file() always writes "row_order = bottom_top"
  if (kv.count("endian")) {
    idx.endian = kv.at("endian") == "little";
  } else {
    // Some real-world WPS_GEOG datasets omit this key entirely (it
    // predates the "endian" field); big-endian is the historical
    // WPS/geogrid default in that case.
    std::fprintf(stderr, "WARNING: index file has no 'endian' key; assuming big-endian.\n");
    idx.endian = false;
  }

  return idx;
}

namespace {

struct TileFileName {
  int ixs, ixe, iys, iye;
};

bool parse_tile_filename(const std::string &name, TileFileName &out) {
  static const std::regex pattern(R"(^(\d{5})-(\d{5})\.(\d{5})-(\d{5})$)");
  std::smatch m;
  if (!std::regex_match(name, m, pattern)) return false;
  out.ixs = std::atoi(m[1].str().c_str());
  out.ixe = std::atoi(m[2].str().c_str());
  out.iys = std::atoi(m[3].str().c_str());
  out.iye = std::atoi(m[4].str().c_str());
  return true;
}

// Inverse of write_binary_tile()'s byte packing: unpacks one wordsize-byte
// word at `barray + wordsize*i` into a signed/unsigned integer per `idx`.
int64_t unpack_word(const unsigned char *barray, long i, const GeogridIndex &idx) {
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

  const unsigned char *w = barray + idx.wordsize * i;
  uint64_t bits = 0;
  int bit_width = idx.wordsize * 8;
  switch (idx.wordsize) {
    case 1:
      bits = w[0];
      break;
    case 2:
      bits = (static_cast<uint64_t>(w[a2]) << 8) | w[b2];
      break;
    case 3:
      bits = (static_cast<uint64_t>(w[a3]) << 16) | (static_cast<uint64_t>(w[b3]) << 8) | w[c3];
      break;
    case 4:
      bits = (static_cast<uint64_t>(w[a4]) << 24) | (static_cast<uint64_t>(w[b4]) << 16) |
             (static_cast<uint64_t>(w[c4]) << 8) | w[d4];
      break;
    default:
      throw GeoConvertError("Unsupported wordsize=" + std::to_string(idx.wordsize));
  }

  if (idx.isigned && (bits & (uint64_t(1) << (bit_width - 1)))) {
    return static_cast<int64_t>(bits) - (int64_t(1) << bit_width);
  }
  return static_cast<int64_t>(bits);
}

} // namespace

std::vector<float> read_tiles(const std::string &dir, GeogridIndex &idx) {
  std::vector<TileFileName> tiles;
  int max_ixe = 0, max_iye = 0;

  {
    std::vector<std::string> names;
    // Enumerate directory entries via the C API to avoid pulling in a
    // <filesystem> dependency here for a single directory listing.
    DIR *d = opendir(dir.c_str());
    if (!d) {
      throw GeoConvertError("Could not open directory '" + dir + "'.");
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
      names.emplace_back(entry->d_name);
    }
    closedir(d);

    for (const auto &name : names) {
      TileFileName t;
      if (!parse_tile_filename(name, t)) continue;
      tiles.push_back(t);
      max_ixe = std::max(max_ixe, t.ixe);
      max_iye = std::max(max_iye, t.iye);
    }
  }

  if (tiles.empty()) {
    throw GeoConvertError("No geogrid tile files found in '" + dir + "'.");
  }

  idx.nx = max_ixe;
  idx.ny = max_iye;

  const int nz = (idx.nz > 0) ? idx.nz : (idx.tz_e - idx.tz_s + 1);
  const int nx_tiles = (idx.nx + idx.tx - 1) / idx.tx;
  const int ny_tiles = (idx.ny + idx.ty - 1) / idx.ty;

  std::vector<float> buffer(static_cast<size_t>(idx.nx) * idx.ny * nz, idx.missing);

  for (int itile_y = 0; itile_y < ny_tiles; ++itile_y) {
    for (int itile_x = 0; itile_x < nx_tiles; ++itile_x) {
      const int ixs = itile_x * idx.tx + 1;
      const int iys = itile_y * idx.ty + 1;
      const int ixe = ixs + idx.tx - 1;
      const int iye = iys + idx.ty - 1;

      char fname[64];
      std::snprintf(fname, sizeof(fname), "%5.5i-%5.5i.%5.5i-%5.5i", ixs, ixe, iys, iye);
      const std::string path = dir + "/" + fname;

      std::ifstream f(path, std::ios::binary);
      if (!f) {
        throw GeoConvertError("Missing expected tile file '" + std::string(fname) + "' in '" + dir + "'.");
      }

      const int tile_nx = idx.tx + 2 * idx.tile_bdr;
      const int tile_ny = idx.ty + 2 * idx.tile_bdr;
      const long tile_count = static_cast<long>(tile_nx) * tile_ny * nz;
      std::vector<unsigned char> raw(static_cast<size_t>(tile_count) * idx.wordsize);
      f.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
      if (!f) {
        throw GeoConvertError("Tile file '" + std::string(fname) + "' is shorter than expected.");
      }

      // Copy only the tile's interior (excluding the tile_bdr border) into
      // the global buffer -- the border is redundant data already covered
      // by neighboring tiles.
      for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < idx.ty; ++y) {
          const int gy = itile_y * idx.ty + y;
          if (gy >= idx.ny) continue;
          for (int x = 0; x < idx.tx; ++x) {
            const int gx = itile_x * idx.tx + x;
            if (gx >= idx.nx) continue;
            const long tile_i = (static_cast<long>(z) * tile_ny + (y + idx.tile_bdr)) * tile_nx +
                                 (x + idx.tile_bdr);
            const int64_t decoded = unpack_word(raw.data(), tile_i, idx);
            const size_t global_i =
                (static_cast<size_t>(z) * idx.ny + gy) * idx.nx + gx;
            buffer[global_i] = static_cast<float>(decoded) * idx.scalefactor;
          }
        }
      }
    }
  }

  return buffer;
}

} // namespace convert_geotiff
