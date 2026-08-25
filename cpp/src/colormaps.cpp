#include "wrftools/colormaps.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace wrftools {
namespace {
using Anchors = std::vector<Rgb>;
const std::map<std::string, Anchors> kAnchors{
    {"viridis", {{68,1,84},{59,82,139},{33,144,140},{93,201,99},{253,231,37}}},
    {"plasma", {{13,8,135},{126,3,168},{204,71,120},{248,149,64},{240,249,33}}},
    {"magma", {{0,0,4},{81,18,124},{183,55,121},{252,137,97},{252,253,191}}},
    {"cividis", {{0,32,76},{58,78,99},{124,123,120},{192,169,102},{255,234,70}}},
    {"coolwarm", {{59,76,192},{146,161,214},{221,221,221},{212,137,116},{180,4,38}}},
    {"terrain", {{51,102,204},{51,204,102},{204,204,102},{153,102,51},{204,204,204},{255,255,255}}},
    {"greys", {{0,0,0},{255,255,255}}},
    {"jet", {{0,0,131},{0,60,170},{5,255,255},{255,255,0},{250,0,0},{128,0,0}}},
};
ColorLut build(const Anchors& anchors) {
    ColorLut result{};
    for (int index = 0; index < 256; ++index) {
        const double position = static_cast<double>(index) / 255.0 * (anchors.size() - 1);
        const auto left = static_cast<std::size_t>(std::floor(position));
        const auto right = std::min(left + 1, anchors.size() - 1);
        const double fraction = position - left;
        for (int channel = 0; channel < 3; ++channel) result[index][channel] = static_cast<unsigned char>(std::lround(anchors[left][channel] + (anchors[right][channel] - anchors[left][channel]) * fraction));
    }
    return result;
}
const std::map<std::string, ColorLut>& luts() { static const auto value = [] { std::map<std::string, ColorLut> result; for (const auto& [name, anchors] : kAnchors) result.emplace(name, build(anchors)); return result; }(); return value; }
}
std::vector<std::string> colormapNames() { std::vector<std::string> result; for (const auto& [name, unused] : kAnchors) { static_cast<void>(unused); result.push_back(name); } return result; }
const ColorLut& colormap(const std::string& name) { if (const auto it = luts().find(name); it != luts().end()) return it->second; throw std::out_of_range("Unknown colormap: " + name); }
std::vector<Rgba> applyColormap(std::span<const float> values, float minimum, float maximum, const ColorLut& lut) {
    std::vector<Rgba> result(values.size(), {0, 0, 0, 0});
    if (!(maximum > minimum)) return result;
    for (std::size_t i = 0; i < values.size(); ++i) if (std::isfinite(values[i])) { const int index = std::clamp(static_cast<int>(std::lround((values[i] - minimum) / (maximum - minimum) * 255)), 0, 255); result[i] = {lut[index][0], lut[index][1], lut[index][2], 255}; }
    return result;
}
}  // namespace wrftools
