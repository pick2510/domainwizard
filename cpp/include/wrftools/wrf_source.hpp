#pragma once

#include "wrftools/wrf_file.hpp"
#include "wrftools/wrf_series.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wrftools {

// Anything a raster layer can be rendered from - a single WrfFile, or
// several combined into one WrfFileSeries. Lets ViewForm/renderLayer treat
// both uniformly, matching wrfseries.py's module docstring: WRFFileSeries
// mirrors WRFFile's read-only surface exactly so callers never need to
// branch on which one they have.
class WrfSource {
public:
    virtual ~WrfSource() = default;
    [[nodiscard]] virtual const std::vector<WrfVariable>& variables() const = 0;
    [[nodiscard]] virtual const std::array<double, 6>& geotransform() const = 0;
    [[nodiscard]] virtual std::array<int, 2> size() const = 0;
    [[nodiscard]] virtual const std::string& projectionWkt() const = 0;
    [[nodiscard]] virtual const GeographicBounds& geographicBounds() const = 0;
    // Real timestamp labels for a series (see WrfFileSeries::times()); a
    // single file has no such labels - the caller falls back to
    // "Step i of N" using the selected variable's own timeCount.
    [[nodiscard]] virtual const std::vector<std::string>* seriesTimes() const = 0;
    [[nodiscard]] virtual std::string displayName() const = 0;
    [[nodiscard]] virtual std::vector<float> read(const std::string& variable, int timeIndex, int levelIndex) = 0;
};

// Opens (or returns the already-open) source for `paths`: one path opens a
// plain WrfFile; several open one WrfFileSeries keyed by the group's first
// (earliest) path. Mirrors rasterlayer.LayerRenderer's open_file/open_files/
// open_paths/invalidate_file - the file-handle cache tier only (no
// slice/image caching yet, see PORT.md).
class WrfSourceRegistry {
public:
    // Throws UserError (via WrfFile/WrfFileSeries) if any path isn't a
    // recognized WRF/WPS file, or the paths don't form one coherent series.
    WrfSource& open(const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::vector<std::string> openPaths() const;
    void invalidate(const std::string& path);
    void clear();

private:
    std::map<std::string, std::unique_ptr<WrfSource>> sources_;
};

}  // namespace wrftools
