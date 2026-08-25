#pragma once

#include "wrftools/colormaps.hpp"
#include "wrftools/crs.hpp"
#include "wrftools/wrf_file.hpp"
#include "wrftools/wrf_source.hpp"

#include <optional>
#include <string>
#include <vector>
#include <QImage>

namespace wrftools {
struct RasterLayer {
    std::string variable;
    int timeIndex{};
    int levelIndex{};
    std::string colormap{"viridis"};
    std::optional<float> minimum;
    std::optional<float> maximum;
    std::string unitKey{"native"};
    double opacity{0.8};
    bool visible{true};
    bool interpolate{true};
};
struct RenderedRaster {
    std::vector<Rgba> pixels;
    int width{};
    int height{};
    float minimum{};
    float maximum{};
    // EPSG:3857 placement bounds of `pixels` - see warp.hpp. Absent for the
    // fixture-literal RenderedRaster construction the widget tests use
    // directly (no WrfFile behind it), where placement isn't under test.
    Bounds2D bounds3857;
    // Populated only when layer.colormap == kCategoricalColormap - what
    // colorbar's categorical legend needs. Empty/default for a continuous
    // layer.
    ColorLut categoricalPalette{};
    std::map<int, std::string> categoricalLabels;
    std::vector<int> presentCategories;
};
[[nodiscard]] RenderedRaster renderLayer(WrfSource& source, const RasterLayer& layer);
[[nodiscard]] QImage rasterImage(const RenderedRaster& raster);
}  // namespace wrftools
