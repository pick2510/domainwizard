#include "wrftools/raster_layer.hpp"
#include "wrftools/error.hpp"
#include "wrftools/units.hpp"
#include "wrftools/warp.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wrftools {
RenderedRaster renderLayer(WrfSource& source, const RasterLayer& layer) {
    const auto& variables = source.variables();
    const auto variable = std::find_if(variables.begin(), variables.end(), [&layer](const WrfVariable& value) { return value.name == layer.variable; });
    if (variable == variables.end()) throw UserError("Layer variable is not available: " + layer.variable);
    const auto native = source.read(layer.variable, layer.timeIndex, layer.levelIndex);
    // Warp in native units, like the Python reference: unit conversion is
    // affine (a straight scale+offset), so it commutes with bilinear
    // resampling and the order makes no visible difference - matching it
    // anyway keeps this pinned against wrftools.rasterlayer's own cache.
    const auto dimensions = source.size();
    auto warped = warpToWebMercator(native, dimensions[0], dimensions[1], source.projectionWkt(), source.geotransform());
    convertInPlace(warped.values, findUnit(variable->units, layer.unitKey));

    if (layer.colormap == kCategoricalColormap) {
        int categoryMin = 0, categoryMax = 0;
        std::vector<int> present;
        {
            std::vector<int> rounded;
            rounded.reserve(warped.values.size());
            for (const auto value : warped.values) if (std::isfinite(value)) rounded.push_back(static_cast<int>(std::lround(value)));
            if (!rounded.empty()) {
                categoryMin = *std::min_element(rounded.begin(), rounded.end());
                categoryMax = *std::max_element(rounded.begin(), rounded.end());
                std::sort(rounded.begin(), rounded.end());
                rounded.erase(std::unique(rounded.begin(), rounded.end()), rounded.end());
                present = std::move(rounded);
            }
        }
        const auto legend = categoricalLut(variable->categoryScheme.value_or(""), categoryMin, categoryMax);
        const auto pixels = applyCategoricalColormap(warped.values, legend.lut);
        return {pixels, warped.width, warped.height, static_cast<float>(categoryMin), static_cast<float>(categoryMax), warped.bounds3857, legend.lut, legend.labels, present};
    }

    float automaticMinimum = std::numeric_limits<float>::infinity(), automaticMaximum = -std::numeric_limits<float>::infinity();
    for (const auto value : warped.values) if (std::isfinite(value)) { automaticMinimum = std::min(automaticMinimum, value); automaticMaximum = std::max(automaticMaximum, value); }
    if (!std::isfinite(automaticMinimum)) { automaticMinimum = 0; automaticMaximum = 1; }
    const float minimum = layer.minimum.value_or(automaticMinimum), maximum = layer.maximum.value_or(automaticMaximum);
    const auto pixels = applyColormap(warped.values, minimum, maximum, colormap(layer.colormap));
    return {pixels, warped.width, warped.height, minimum, maximum, warped.bounds3857, {}, {}, {}};
}

QImage rasterImage(const RenderedRaster& raster) {
    QImage image(raster.width, raster.height, QImage::Format_RGBA8888);
    for (int y = 0; y < raster.height; ++y) {
        auto* line = image.scanLine(y);
        for (int x = 0; x < raster.width; ++x) {
            const auto& color = raster.pixels[static_cast<std::size_t>(y) * raster.width + x];
            line[x * 4] = color[0]; line[x * 4 + 1] = color[1]; line[x * 4 + 2] = color[2]; line[x * 4 + 3] = color[3];
        }
    }
    return image;
}
}  // namespace wrftools
