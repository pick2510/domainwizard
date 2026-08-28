#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace wrftools {

// Summary statistics over a set of finite values - NaN/Inf entries (missing/
// nodata pixels, or a timeseries gap) are silently skipped, matching every
// other range/min-max computation in this app (see raster_layer.cpp,
// ViewForm::computeSeriesRange). count is the number of finite values
// actually used, not values.size().
struct DescriptiveStats {
    std::size_t count{};
    float minimum{};
    float maximum{};
    float mean{};
    float median{};
    float stddev{};  // population stddev (divide by count, not count-1) - a
                      // full raster/timeseries here is the whole population
                      // being described, not a sample of some larger one.
};

// Throws UserError if `values` contains no finite entries.
[[nodiscard]] DescriptiveStats computeDescriptiveStats(std::span<const float> values);

// A fixed-bin-count histogram over the finite values in `values`, for a
// quick distribution plot. binCounts.size() == binCount always; an all-
// identical-value input still returns one non-empty bin (binWidth 0 is
// avoided by widening to +-0.5 around the single value).
struct Histogram {
    std::vector<std::size_t> binCounts;
    float binStart{};
    float binWidth{};
};

// Throws UserError if `values` contains no finite entries. binCount must be
// >= 1.
[[nodiscard]] Histogram computeHistogram(std::span<const float> values, int binCount = 20);

}  // namespace wrftools
