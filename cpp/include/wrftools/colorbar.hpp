#pragma once

#include "wrftools/colormaps.hpp"

#include <QPixmap>
#include <map>
#include <string>
#include <vector>

namespace wrftools {
// tickFormat is one of "auto" (%.3g, decimals ignored), "fixed" (%.*f), or
// "scientific" (%.*e). Mirrors colorbar.py's _format_tick.
[[nodiscard]] QPixmap buildColorbar(const std::string& title, float minimum, float maximum, const ColorLut& lut,
    int tickCount = 3, const std::string& tickFormat = "auto", int tickDecimals = 2);

// A swatch+label legend for a categorical layer - present is the list of
// category values actually found in the data (already filtered/sorted by
// the caller), capped at 20 rows with a trailing "+N more" row. Mirrors
// colorbar.py's build_categorical_legend_pixmap.
[[nodiscard]] QPixmap buildCategoricalLegend(const ColorLut& lut, const std::map<int, std::string>& labels,
    const std::vector<int>& present, const std::string& title);
}  // namespace wrftools
