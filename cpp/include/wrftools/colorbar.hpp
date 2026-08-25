#pragma once

#include "wrftools/colormaps.hpp"

#include <QPixmap>
#include <string>

namespace wrftools {
[[nodiscard]] QPixmap buildColorbar(const std::string& title, float minimum, float maximum, const ColorLut& lut, int tickCount = 3);
}  // namespace wrftools
