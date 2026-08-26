#pragma once

#include "wrftools/wrf_source.hpp"

#include <filesystem>

namespace wrftools {

// True when `path` is a directory holding a WPS_GEOG binary dataset (an
// "index" metadata file alongside its numbered tile files) - the static
// geographical-data format geogrid.exe reads/writes, e.g.
// modis_landuse_20class_30s. Used by WrfSourceRegistry::open to route a
// directory path here instead of treating it as a WRF/WPS NetCDF file.
[[nodiscard]] bool isWpsGeogDataset(const std::filesystem::path& path);

// Read-only WrfSource backed by a WPS_GEOG binary dataset directory. The
// Python reference only ever reads such a dataset's "index" file for
// Geogrid.tbl resolution metadata (see wps_binary_index.py) - it never
// visualizes the actual tile data, unlike this class. Exposes the dataset
// as a single variable (its own description, one z-level per read()) so it
// slots into the existing WrfSource/LayerRenderer/ViewForm pipeline
// unchanged, the same way WrfFile does for NetCDF.
class WpsBinarySource final : public WrfSource {
public:
    explicit WpsBinarySource(std::filesystem::path directory);
    [[nodiscard]] const std::vector<WrfVariable>& variables() const override { return variables_; }
    [[nodiscard]] const std::array<double, 6>& geotransform() const override { return geotransform_; }
    [[nodiscard]] std::array<int, 2> size() const override { return size_; }
    [[nodiscard]] const std::string& projectionWkt() const override { return projectionWkt_; }
    [[nodiscard]] const GeographicBounds& geographicBounds() const override { return geographicBounds_; }
    [[nodiscard]] const std::vector<std::string>* seriesTimes() const override { return nullptr; }
    [[nodiscard]] std::string displayName() const override;
    [[nodiscard]] std::vector<float> read(const std::string& variable, int timeIndex, int levelIndex) override;

private:
    std::filesystem::path directory_;
    std::vector<WrfVariable> variables_;
    std::array<double, 6> geotransform_{};
    std::array<int, 2> size_{};
    GeographicBounds geographicBounds_{};
    std::string projectionWkt_;
    // nx*ny*nz, top-down (row 0 = north, matching WrfFile's convention),
    // z-major - reordered once in the constructor from the tile reader's
    // own row order (see .cpp) so read() is a plain contiguous slice.
    std::vector<float> buffer_;
};

}  // namespace wrftools
