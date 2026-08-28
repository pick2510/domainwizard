#pragma once

#include "wrftools/stats.hpp"

#include <optional>
#include <QString>
#include <QWidget>
#include <vector>

namespace wrftools {

// A small hand-rolled line-chart widget for a value-over-time series -
// matches this project's existing preference for a lightweight, dependency-
// free QPainter widget (see tile_map_widget.cpp/colorbar.cpp) over pulling
// in Qt Charts for what is otherwise a single simple plot.
class LineChartWidget final : public QWidget {
public:
    explicit LineChartWidget(QWidget* parent = nullptr);
    // labels.size() must equal values.size(). A nullopt entry (a timestep
    // where the point fell outside the raster, or was nodata) is skipped -
    // the line breaks around it rather than interpolating across a gap.
    void setSeries(std::vector<QString> labels, std::vector<std::optional<float>> values, QString yAxisLabel);
    [[nodiscard]] QSize sizeHint() const override { return {520, 280}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<QString> labels_;
    std::vector<std::optional<float>> values_;
    QString yAxisLabel_;
};

// A small hand-rolled bar-chart widget for a Histogram (stats.hpp).
class HistogramChartWidget final : public QWidget {
public:
    explicit HistogramChartWidget(QWidget* parent = nullptr);
    void setHistogram(Histogram histogram, QString xAxisLabel);
    [[nodiscard]] QSize sizeHint() const override { return {520, 280}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Histogram histogram_;
    QString xAxisLabel_;
};

}  // namespace wrftools
