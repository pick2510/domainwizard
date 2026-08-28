#include "wrftools/stats.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <cmath>

namespace wrftools {
namespace {
std::vector<float> finiteValues(std::span<const float> values) {
    std::vector<float> finite;
    finite.reserve(values.size());
    for (const auto value : values) if (std::isfinite(value)) finite.push_back(value);
    return finite;
}
}  // namespace

DescriptiveStats computeDescriptiveStats(std::span<const float> values) {
    auto finite = finiteValues(values);
    if (finite.empty()) throw UserError("No finite values to compute statistics from.");

    DescriptiveStats stats;
    stats.count = finite.size();
    double sum = 0.0;
    stats.minimum = finite.front();
    stats.maximum = finite.front();
    for (const auto value : finite) {
        sum += value;
        stats.minimum = std::min(stats.minimum, value);
        stats.maximum = std::max(stats.maximum, value);
    }
    stats.mean = static_cast<float>(sum / static_cast<double>(finite.size()));

    double variance = 0.0;
    for (const auto value : finite) { const double delta = value - stats.mean; variance += delta * delta; }
    stats.stddev = static_cast<float>(std::sqrt(variance / static_cast<double>(finite.size())));

    std::sort(finite.begin(), finite.end());
    const std::size_t mid = finite.size() / 2;
    stats.median = finite.size() % 2 == 0 ? (finite[mid - 1] + finite[mid]) / 2.0f : finite[mid];
    return stats;
}

Histogram computeHistogram(std::span<const float> values, int binCount) {
    if (binCount < 1) throw UserError("A histogram needs at least one bin.");
    auto finite = finiteValues(values);
    if (finite.empty()) throw UserError("No finite values to compute a histogram from.");

    auto [minIt, maxIt] = std::minmax_element(finite.begin(), finite.end());
    float minimum = *minIt, maximum = *maxIt;
    if (minimum == maximum) { minimum -= 0.5f; maximum += 0.5f; }  // a single distinct value still gets one visible bin

    Histogram histogram;
    histogram.binCounts.assign(static_cast<std::size_t>(binCount), 0);
    histogram.binStart = minimum;
    histogram.binWidth = (maximum - minimum) / static_cast<float>(binCount);
    for (const auto value : finite) {
        int bin = static_cast<int>((value - minimum) / histogram.binWidth);
        bin = std::clamp(bin, 0, binCount - 1);
        ++histogram.binCounts[static_cast<std::size_t>(bin)];
    }
    return histogram;
}

}  // namespace wrftools
