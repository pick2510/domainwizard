#include "wrftools/layer_renderer.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <numeric>

namespace wrftools {
namespace {
std::size_t sliceBytes(const WarpedRaster& raster) { return raster.values.size() * sizeof(float); }
}

LayerRenderer::LayerRenderer(std::size_t sliceCacheBytes, std::size_t imageCacheSize)
    : sliceCacheBytes_(sliceCacheBytes), imageCacheSize_(imageCacheSize) {}

void LayerRenderer::invalidateFile(const std::string& path) {
    registry_.invalidate(path);
    sliceBytesUsed_ -= std::accumulate(sliceCache_.begin(), sliceCache_.end(), std::size_t{0},
        [&path](std::size_t total, const SliceEntry& entry) { return total + (entry.key.filePath == path ? sliceBytes(entry.raster) : 0); });
    sliceCache_.erase(std::remove_if(sliceCache_.begin(), sliceCache_.end(), [&path](const SliceEntry& entry) { return entry.key.filePath == path; }), sliceCache_.end());
    imageCache_.erase(std::remove_if(imageCache_.begin(), imageCache_.end(), [&path](const ImageEntry& entry) { return entry.key.slice.filePath == path; }), imageCache_.end());
}

void LayerRenderer::clear() {
    registry_.clear();
    sliceCache_.clear();
    imageCache_.clear();
    sliceBytesUsed_ = 0;
    stats_ = {};
}

void LayerRenderer::evictSlicesIfNeeded() {
    while (sliceBytesUsed_ > sliceCacheBytes_ && sliceCache_.size() > 1) {
        sliceBytesUsed_ -= sliceBytes(sliceCache_.front().raster);
        sliceCache_.erase(sliceCache_.begin());
    }
}

const WarpedRaster& LayerRenderer::getSlice(const std::string& filePath, const RasterLayer& layer) {
    const SliceKey key{filePath, layer.variable, layer.timeIndex, layer.levelIndex};
    if (const auto found = std::find_if(sliceCache_.begin(), sliceCache_.end(), [&key](const SliceEntry& entry) { return entry.key == key; }); found != sliceCache_.end()) {
        auto entry = std::move(*found);
        sliceCache_.erase(found);
        sliceCache_.push_back(std::move(entry));  // move-to-back: most-recently-used
        ++stats_.sliceHits;
        return sliceCache_.back().raster;
    }
    ++stats_.sliceMisses;

    auto& source = registry_.open({filePath});
    const auto& variables = source.variables();
    const auto variable = std::find_if(variables.begin(), variables.end(), [&layer](const WrfVariable& value) { return value.name == layer.variable; });
    if (variable == variables.end()) throw UserError("Layer variable is not available: " + layer.variable);
    const auto native = source.read(layer.variable, layer.timeIndex, layer.levelIndex);
    const auto dimensions = source.size();
    auto warped = warpToWebMercator(native, dimensions[0], dimensions[1], source.projectionWkt(), source.geotransform());
    sliceBytesUsed_ += sliceBytes(warped);
    sliceCache_.push_back({key, std::move(warped)});
    evictSlicesIfNeeded();
    return sliceCache_.back().raster;
}

RenderedRaster LayerRenderer::render(const std::string& filePath, const RasterLayer& layer) {
    const SliceKey sliceKey{filePath, layer.variable, layer.timeIndex, layer.levelIndex};
    const ImageKey imageKey{sliceKey, layer.colormap, layer.minimum, layer.maximum, layer.unitKey};
    if (const auto found = std::find_if(imageCache_.begin(), imageCache_.end(), [&imageKey](const ImageEntry& entry) { return entry.key == imageKey; }); found != imageCache_.end()) {
        auto entry = std::move(*found);
        imageCache_.erase(found);
        imageCache_.push_back(std::move(entry));
        ++stats_.imageHits;
        return imageCache_.back().raster;
    }
    ++stats_.imageMisses;

    const auto& warped = getSlice(filePath, layer);
    auto& source = registry_.open({filePath});
    const auto& variables = source.variables();
    const auto variable = std::find_if(variables.begin(), variables.end(), [&layer](const WrfVariable& value) { return value.name == layer.variable; });
    if (variable == variables.end()) throw UserError("Layer variable is not available: " + layer.variable);
    auto rendered = colorizeWarped(warped, layer, *variable);

    imageCache_.push_back({imageKey, rendered});
    while (imageCache_.size() > imageCacheSize_) imageCache_.erase(imageCache_.begin());
    return rendered;
}

}  // namespace wrftools
