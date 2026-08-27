#pragma once

#include "wrftools/raster_layer.hpp"
#include "wrftools/warp.hpp"
#include "wrftools/wrf_source.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wrftools {

// Default byte budget for the warped-slice cache (tier 2, the expensive
// one - a GDAL read + GDALWarp reprojection) and entry-count budget for the
// colormapped-image cache (tier 3, cheap to rebuild from tier 2). Mirrors
// wrftools.rasterlayer's DEFAULT_SLICE_CACHE_BYTES/DEFAULT_IMAGE_CACHE_SIZE.
constexpr std::size_t kDefaultSliceCacheBytes = 256 * 1024 * 1024;
constexpr std::size_t kDefaultImageCacheSize = 48;

// Renders RasterLayers into RenderedRasters, with a two-tier LRU cache: an
// open-file registry (WrfSourceRegistry), a byte-bounded cache of warped
// (EPSG:3857, native-unit) slices keyed by (file, variable, time, level),
// and a count-bounded cache of colormapped images keyed by the slice key
// plus (colormap, vmin, vmax, unit). Opacity/visible/interpolate are
// deliberately excluded from both cache keys - see RasterLayer - so those
// toggles never invalidate anything. Mirrors wrftools.rasterlayer.LayerRenderer.
class LayerRenderer {
public:
    // Cache-tier hit/miss counters, mirroring wrftools.rasterlayer.
    // LayerRenderer.stats - lets a test (or a future diagnostics view) pin
    // the caching contract directly instead of only its externally visible
    // effect (same pixels back, no crash).
    struct Stats {
        int sliceHits{};
        int sliceMisses{};
        int imageHits{};
        int imageMisses{};
    };

    explicit LayerRenderer(std::size_t sliceCacheBytes = kDefaultSliceCacheBytes, std::size_t imageCacheSize = kDefaultImageCacheSize);

    [[nodiscard]] WrfSource& openFile(const std::vector<std::filesystem::path>& paths) { return registry_.open(paths); }
    [[nodiscard]] std::vector<std::string> openPaths() const { return registry_.openPaths(); }
    // Closes a file and drops every cached slice/image that came from it -
    // for an explicit close/reload action, not automatic mtime polling.
    void invalidateFile(const std::string& path);
    void clear();

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    // Renders `layer` against the already-open file at `filePath`. Throws
    // UserError (via WrfSource::read) for a bad time/level index on an
    // otherwise-valid variable - that's a caller bug, not something to
    // degrade gracefully. The caller is expected to have opened filePath
    // via openFile() first.
    [[nodiscard]] RenderedRaster render(const std::string& filePath, const RasterLayer& layer);

    // Warms the slice cache for `layer` without building a colormapped
    // image - mirrors rasterlayer.LayerRenderer.prefetch, used to warm the
    // next/previous timestep of a series ahead of the user stepping to it.
    void prefetch(const std::string& filePath, const RasterLayer& layer) { static_cast<void>(getSlice(filePath, layer)); }

    // Samples the already-warped (EPSG:3857) slice for `layer` at a lon/lat
    // point, converted to `layer`'s displayed unit - what a mouse-hover
    // readout needs. Returns nullopt when the point falls outside the
    // raster's bounds or lands on a nodata (NaN) pixel. Goes through the
    // same slice cache render()/prefetch() populate, so a hover over an
    // already-rendered layer never re-reads or re-warps.
    [[nodiscard]] std::optional<double> valueAt(const std::string& filePath, const RasterLayer& layer, LonLat point);

private:
    struct SliceKey {
        std::string filePath;
        std::string variable;
        int timeIndex{};
        int levelIndex{};
        [[nodiscard]] bool operator==(const SliceKey&) const = default;
    };
    struct SliceEntry {
        SliceKey key;
        WarpedRaster raster;
    };
    struct ImageKey {
        SliceKey slice;
        std::string colormap;
        std::optional<float> minimum;
        std::optional<float> maximum;
        std::string unitKey;
        [[nodiscard]] bool operator==(const ImageKey&) const = default;
    };
    struct ImageEntry {
        ImageKey key;
        RenderedRaster raster;
    };

    [[nodiscard]] const WarpedRaster& getSlice(const std::string& filePath, const RasterLayer& layer);
    void evictSlicesIfNeeded();

    WrfSourceRegistry registry_;
    Stats stats_;
    std::size_t sliceCacheBytes_;
    std::size_t imageCacheSize_;
    std::size_t sliceBytesUsed_{0};
    // Small caches (a handful of open layers/timesteps in practice), so a
    // flat MRU-at-back vector scanned linearly beats the bookkeeping of a
    // hash map keyed on a multi-field struct.
    std::vector<SliceEntry> sliceCache_;
    std::vector<ImageEntry> imageCache_;
};

}  // namespace wrftools
