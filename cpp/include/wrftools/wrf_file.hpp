#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

class GDALDataset;

namespace wrftools {

struct WrfVariable {
    std::string name;
    std::string description;
    std::string units;
    std::optional<std::string> extraDimension;
    int timeCount{1};
    int levelCount{1};
    std::optional<std::string> categoryScheme;
};
struct GeographicBounds { double west{}; double south{}; double east{}; double north{}; };

// Pinned separately from WrfFile's constructor - see wrf_file.cpp's own
// comment - because it's the one piece of subdataset-target parsing that
// differs between GDAL's netCDF and HDF5 drivers and is otherwise only
// exercised incidentally through whichever driver GDAL happens to pick for
// a given real file.
[[nodiscard]] std::string subdatasetVariableName(const std::string& target);

// Read-only GDAL-backed WRF/WPS source. Raster read/warp is deliberately a
// separate renderer concern; this class owns file discovery and metadata.
class WrfFile {
public:
    explicit WrfFile(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<WrfVariable>& variables() const noexcept { return variables_; }
    [[nodiscard]] const std::array<double, 6>& geotransform() const noexcept { return geotransform_; }
    [[nodiscard]] std::array<int, 2> size() const noexcept { return size_; }
    [[nodiscard]] const GeographicBounds& geographicBounds() const noexcept { return geographicBounds_; }
    [[nodiscard]] const std::string& projectionWkt() const noexcept { return projectionWkt_; }
    [[nodiscard]] std::vector<float> read(const std::string& variable, int timeIndex = 0, int levelIndex = 0) const;

private:
    std::filesystem::path path_;
    std::string driverName_;
    std::vector<WrfVariable> variables_;
    std::array<double, 6> geotransform_{};
    std::array<int, 2> size_{};
    GeographicBounds geographicBounds_{};
    std::string projectionWkt_;
    std::unordered_map<std::string, int> staggerAxis_;
    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> dataset_{nullptr, nullptr};
};

}  // namespace wrftools
