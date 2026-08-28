#pragma once

#include <optional>
#include <QDialog>
#include <QString>
#include <vector>

namespace wrftools {

// Non-modal ("new window", per the feature request) popup showing a
// variable's value at one map point across every time step of the open
// file/series, plus descriptive statistics over that timeseries - opened by
// ViewForm when the user clicks a raster pixel in a layer whose source has
// more than one time step. Closes and deletes itself (WA_DeleteOnClose);
// several can be open at once, one per click.
class TimeSeriesDialog final : public QDialog {
    Q_OBJECT
public:
    // labels.size() must equal values.size(); a nullopt entry (point fell
    // outside the raster, or was nodata, at that time step) is skipped by
    // both the stats and the plotted line - see LineChartWidget.
    TimeSeriesDialog(const QString& title, std::vector<QString> timeLabels, std::vector<std::optional<float>> values,
        const QString& unitLabel, QWidget* parent = nullptr);
};

// Same idea for a single-timestep file: the whole raster's values (at the
// layer's current level) rather than one point's values over time -
// descriptive statistics plus a distribution histogram.
class RasterStatsDialog final : public QDialog {
    Q_OBJECT
public:
    RasterStatsDialog(const QString& title, const std::vector<float>& values, const QString& unitLabel, QWidget* parent = nullptr);
};

}  // namespace wrftools
