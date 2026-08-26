#pragma once

#include <array>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace wrftools {
using Rgb = std::array<unsigned char, 3>;
using Rgba = std::array<unsigned char, 4>;
using ColorLut = std::array<Rgb, 256>;
[[nodiscard]] std::vector<std::string> colormapNames();
[[nodiscard]] const ColorLut& colormap(const std::string& name);
[[nodiscard]] std::vector<Rgba> applyColormap(std::span<const float> values, float minimum, float maximum, const ColorLut& lut);

// Sentinel colormap name for a categorical (class-index) variable, kept out
// of colormapNames() - a UI adds it as its own combo entry, only for
// variables that actually carry a categoryScheme. Mirrors
// wrftools.colormaps.CATEGORICAL.
inline constexpr const char* kCategoricalColormap = "categorical";

struct CategoricalLegend {
    ColorLut lut{};
    std::map<int, std::string> labels;
};

// Real WRF land-use/soil palette+label lookup for `scheme` (WRF's MMINLU
// global attribute, e.g. "USGS" or "MODIFIED_IGBP_MODIS_NOAH"), covering
// category values in [max(0, categoryMin), min(255, categoryMax)]. An
// unknown value (unrecognized scheme, or a value the scheme's table doesn't
// define - e.g. any soil-type scheme, which has no table here) gets a
// deterministic 8-color fallback cycle and a "Category N" label rather than
// failing. Mirrors wrftools.colormaps.categorical_lut.
[[nodiscard]] CategoricalLegend categoricalLut(const std::string& scheme, int categoryMin, int categoryMax);
[[nodiscard]] std::vector<Rgba> applyCategoricalColormap(std::span<const float> values, const ColorLut& lut);
}  // namespace wrftools
