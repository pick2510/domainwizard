#include "wrftools/raster_layer.hpp"
#include "wrftools/error.hpp"
#include "wrftools/units.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wrftools {
RenderedRaster renderLayer(const WrfFile& file, const RasterLayer& layer) {
    const auto variable = std::find_if(file.variables().begin(), file.variables().end(), [&layer](const WrfVariable& value) { return value.name == layer.variable; });
    if (variable == file.variables().end()) throw UserError("Layer variable is not available: " + layer.variable);
    auto values = file.read(layer.variable, layer.timeIndex, layer.levelIndex);
    convertInPlace(values, findUnit(variable->units, layer.unitKey));
    float automaticMinimum = std::numeric_limits<float>::infinity(), automaticMaximum = -std::numeric_limits<float>::infinity();
    for (const auto value : values) if (std::isfinite(value)) { automaticMinimum = std::min(automaticMinimum, value); automaticMaximum = std::max(automaticMaximum, value); }
    if (!std::isfinite(automaticMinimum)) { automaticMinimum = 0; automaticMaximum = 1; }
    const float minimum = layer.minimum.value_or(automaticMinimum), maximum = layer.maximum.value_or(automaticMaximum);
    const auto dimensions = file.size();
    return {applyColormap(values, minimum, maximum, colormap(layer.colormap)), dimensions[0], dimensions[1], minimum, maximum};
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
