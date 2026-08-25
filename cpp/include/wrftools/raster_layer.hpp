#pragma once

#include "wrftools/colormaps.hpp"
#include "wrftools/wrf_file.hpp"

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
};
[[nodiscard]] RenderedRaster renderLayer(const WrfFile& file, const RasterLayer& layer);
[[nodiscard]] QImage rasterImage(const RenderedRaster& raster);
}  // namespace wrftools
