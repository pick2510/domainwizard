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
namespace {
// Deterministic ColorBrewer-Paired-style cycle for any category value a
// scheme's own table doesn't define (an unrecognized scheme entirely - e.g.
// any soil-type scheme, which has no table below - or a value outside the
// known range). Matches wrftools.colormaps._FALLBACK_CATEGORY_COLORS.
constexpr std::array<Rgb, 8> kFallbackCategoryColors{{
    {166, 206, 227}, {31, 120, 180}, {178, 223, 138}, {51, 160, 44},
    {251, 154, 153}, {227, 26, 28}, {253, 191, 111}, {255, 127, 0},
}};

// value -> (label, "#RRGGBB"). Values and colors are WRF's own LANDUSE.TBL,
// vendored via gis4wrf.core.readers.categories.LANDUSE - values 29/30 are
// genuinely absent from USGS (not a transcription gap).
const std::map<std::string, std::map<int, std::pair<std::string, Rgb>>>& landuseTables() {
    static const std::map<std::string, std::map<int, std::pair<std::string, Rgb>>> value{
        {"USGS", {
            {1, {"Urban and Built-Up Land", {0xFF, 0x00, 0x00}}},
            {2, {"Dryland Cropland and Pasture", {0xFF, 0xFF, 0x00}}},
            {3, {"Irrigated Cropland and Pasture", {0xFF, 0xF0, 0x54}}},
            {4, {"Mixed Dryland/Irrigated Cropland and Pasture", {0xF9, 0xFF, 0x56}}},
            {5, {"Cropland/Grassland Mosaic", {0xDE, 0xFF, 0x68}}},
            {6, {"Cropland/Woodland Mosaic", {0xFF, 0xE3, 0x6B}}},
            {7, {"Grassland", {0xFF, 0x99, 0x00}}},
            {8, {"Shrubland", {0x99, 0x33, 0x66}}},
            {9, {"Mixed Shrubland/Grassland", {0xFF, 0xCC, 0x99}}},
            {10, {"Savanna", {0xFF, 0xCC, 0x00}}},
            {11, {"Deciduous Broadleaf Forest", {0x99, 0xFF, 0x99}}},
            {12, {"Deciduous Needleleaf Forest", {0x99, 0xCC, 0x00}}},
            {13, {"Evergreen Broadleaf Forest", {0x00, 0xFF, 0x00}}},
            {14, {"Evergreen Needleleaf Forest", {0x00, 0x80, 0x00}}},
            {15, {"Mixed Forest", {0x33, 0x99, 0x66}}},
            {16, {"Water Bodies", {0x00, 0x00, 0x80}}},
            {17, {"Herbaceous Wetland", {0x00, 0x82, 0x99}}},
            {18, {"Wooded Wetland", {0x00, 0x66, 0x99}}},
            {19, {"Barren or Sparsely Vegetated", {0x80, 0x80, 0x80}}},
            {20, {"Herbaceous Tundra", {0x37, 0x87, 0x54}}},
            {21, {"Wooded Tundra", {0x00, 0x88, 0x33}}},
            {22, {"Mixed Tundra", {0x4A, 0x87, 0x60}}},
            {23, {"Bare Ground Tundra", {0x74, 0x87, 0x60}}},
            {24, {"Snow or Ice", {0xBA, 0xEC, 0xFF}}},
            {25, {"Playa", {0xC2, 0xDF, 0xA9}}},
            {26, {"Lava", {0xC2, 0x3E, 0x29}}},
            {27, {"White Sand", {0xDE, 0xE8, 0xCD}}},
            {28, {"Lake", {0x00, 0x00, 0xFF}}},
            {31, {"Low Intensity Residential", {0x68, 0x68, 0x68}}},
            {32, {"High Intensity Residential", {0x51, 0x51, 0x51}}},
            {33, {"Industrial or Commercial", {0x2D, 0x2D, 0x2D}}},
        }},
        {"MODIFIED_IGBP_MODIS_NOAH", {
            {1, {"Evergreen Needleleaf Forest", {0x00, 0x80, 0x00}}},
            {2, {"Evergreen Broadleaf Forest", {0x00, 0xFF, 0x00}}},
            {3, {"Deciduous Needleleaf Forest", {0x99, 0xCC, 0x00}}},
            {4, {"Deciduous Broadleaf Forest", {0x99, 0xFF, 0x99}}},
            {5, {"Mixed Forests", {0x33, 0x99, 0x66}}},
            {6, {"Closed Shrublands", {0x99, 0x33, 0x66}}},
            {7, {"Open Shrublands", {0xFF, 0xCC, 0x99}}},
            {8, {"Woody Savannas", {0xCC, 0xFF, 0xCC}}},
            {9, {"Savannas", {0xFF, 0xCC, 0x00}}},
            {10, {"Grasslands", {0xFF, 0x99, 0x00}}},
            {11, {"Permanent wetlands", {0x00, 0x66, 0x99}}},
            {12, {"Croplands", {0xFF, 0xFF, 0x00}}},
            {13, {"Urban and Built-Up", {0xFF, 0x00, 0x00}}},
            {14, {"Cropland/Natural Vegetation Mosaic", {0x99, 0x99, 0x66}}},
            {15, {"Snow and Ice", {0xBA, 0xEC, 0xFF}}},
            {16, {"Barren or Sparsely Vegetated", {0x80, 0x80, 0x80}}},
            {17, {"Water", {0x00, 0x00, 0x80}}},
            {18, {"Wooded Tundra", {0x00, 0x88, 0x33}}},
            {19, {"Mixed Tundra", {0x4A, 0x87, 0x60}}},
            {20, {"Barren Tundra", {0x74, 0x87, 0x60}}},
            {21, {"Lake", {0x00, 0x00, 0xFF}}},
            {31, {"Low Intensity Residential", {0x68, 0x68, 0x68}}},
            {32, {"High Intensity Residential", {0x51, 0x51, 0x51}}},
            {33, {"Industrial or Commercial", {0x2D, 0x2D, 0x2D}}},
        }},
    };
    return value;
}
}  // namespace

CategoricalLegend categoricalLut(const std::string& scheme, int categoryMin, int categoryMax) {
    CategoricalLegend result{};
    const auto tables = landuseTables();
    const auto known = tables.find(scheme);
    const auto* table = known != tables.end() ? &known->second : nullptr;
    for (int value = std::max(0, categoryMin); value <= std::min(255, categoryMax); ++value) {
        const auto entry = table ? table->find(value) : std::map<int, std::pair<std::string, Rgb>>::const_iterator{};
        if (table && entry != table->end()) {
            result.lut[value] = entry->second.second;
            result.labels[value] = entry->second.first;
        } else {
            result.lut[value] = kFallbackCategoryColors[static_cast<std::size_t>(value) % kFallbackCategoryColors.size()];
            result.labels[value] = "Category " + std::to_string(value);
        }
    }
    return result;
}
std::vector<Rgba> applyCategoricalColormap(std::span<const float> values, const ColorLut& lut) {
    std::vector<Rgba> result(values.size(), {0, 0, 0, 0});
    for (std::size_t i = 0; i < values.size(); ++i) if (std::isfinite(values[i])) { const int index = static_cast<int>(std::lround(values[i])); if (index >= 0 && index < 256) result[i] = {lut[index][0], lut[index][1], lut[index][2], 255}; }
    return result;
}
}  // namespace wrftools
