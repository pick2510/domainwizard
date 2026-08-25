#include "wrftools/units.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

namespace wrftools {
namespace {
std::string normalized(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::istringstream input(value); std::string word, result;
    while (input >> word) { if (!result.empty()) result += ' '; result += word; }
    if (result == "m/s" || result == "ms-1" || result == "m.s-1") return "m s-1";
    return result;
}
const std::map<std::string, std::vector<Unit>> kConversions{
    {"k", {{"degC", "°C", 1, -273.15}, {"degF", "°F", 9.0 / 5.0, -459.67}}},
    {"m s-1", {{"kmh", "km/h", 3.6, 0}, {"kn", "knots", 1.9438444924406046, 0}, {"mph", "mph", 2.2369362920544025, 0}}},
    {"pa", {{"hpa", "hPa", 0.01, 0}, {"inhg", "inHg", 1.0 / 3386.389, 0}}},
    {"m", {{"ft", "ft", 3.280839895013123, 0}, {"km", "km", 0.001, 0}}},
    {"mm", {{"in", "in", 1.0 / 25.4, 0}}},
    {"kg m-2", {{"in", "in", 1.0 / 25.4, 0}}},
};
}
std::vector<Unit> conversionsFor(const std::string& nativeUnits) {
    std::vector<Unit> result{{"native", nativeUnits, 1, 0}};
    if (const auto it = kConversions.find(normalized(nativeUnits)); it != kConversions.end()) result.insert(result.end(), it->second.begin(), it->second.end());
    return result;
}
Unit findUnit(const std::string& nativeUnits, const std::string& key) {
    const auto choices = conversionsFor(nativeUnits);
    const auto found = std::find_if(choices.begin(), choices.end(), [&key](const Unit& item) { return item.key == key; });
    if (found == choices.end()) throw std::out_of_range("Unknown target unit: " + key);
    return *found;
}
double convert(double value, const Unit& unit) { return value * unit.scale + unit.offset; }
void convertInPlace(std::span<float> values, const Unit& unit) { for (auto& value : values) value = static_cast<float>(convert(value, unit)); }
}  // namespace wrftools
