#pragma once

#include <array>
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
}  // namespace wrftools
