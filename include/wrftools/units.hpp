#pragma once

#include <span>
#include <string>
#include <vector>

namespace wrftools {
struct Unit {
    std::string key;
    std::string label;
    double scale{1.0};
    double offset{};
};
[[nodiscard]] std::vector<Unit> conversionsFor(const std::string& nativeUnits);
[[nodiscard]] Unit findUnit(const std::string& nativeUnits, const std::string& key);
[[nodiscard]] double convert(double value, const Unit& unit);
void convertInPlace(std::span<float> values, const Unit& unit);
}  // namespace wrftools
