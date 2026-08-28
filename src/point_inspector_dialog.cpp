#include "wrftools/point_inspector_dialog.hpp"
#include "wrftools/chart_widget.hpp"
#include "wrftools/stats.hpp"

#include <QLabel>
#include <QVBoxLayout>

namespace wrftools {
namespace {
QString statsSummaryText(const DescriptiveStats& stats, const QString& unitLabel) {
    const QString unit = unitLabel.isEmpty() ? QString() : " " + unitLabel;
    // Each placeholder is used exactly once, numbered in the same
    // left-to-right order the .arg() calls fill them in - unlike a shared
    // "%N" reused after every number (relying on QString::arg() always
    // targeting the lowest still-unfilled placeholder regardless of call
    // order), reordering these calls to match a future reordering of the
    // labels can't silently swap two values.
    const auto number = [](double value) { return QString::number(value, 'g', 6); };
    return QString("Count: %1    Min: %2    Max: %3    Mean: %4    Median: %5    Std Dev: %6")
        .arg(stats.count)
        .arg(number(stats.minimum) + unit)
        .arg(number(stats.maximum) + unit)
        .arg(number(stats.mean) + unit)
        .arg(number(stats.median) + unit)
        .arg(number(stats.stddev) + unit);
}

QLabel* statsLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
}  // namespace

TimeSeriesDialog::TimeSeriesDialog(const QString& title, std::vector<QString> timeLabels, std::vector<std::optional<float>> values,
    const QString& unitLabel, QWidget* parent) : QDialog(parent) {
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);

    std::vector<float> finiteValues;
    finiteValues.reserve(values.size());
    for (const auto& value : values) if (value) finiteValues.push_back(*value);

    auto* layout = new QVBoxLayout(this);
    auto* stats = statsLabel(this);
    stats->setText(finiteValues.empty() ? "No data at this point in any time step." : statsSummaryText(computeDescriptiveStats(finiteValues), unitLabel));
    layout->addWidget(stats);

    auto* chart = new LineChartWidget(this);
    chart->setSeries(std::move(timeLabels), std::move(values), unitLabel);
    layout->addWidget(chart);
    resize(chart->sizeHint().width(), chart->sizeHint().height() + 60);
}

RasterStatsDialog::RasterStatsDialog(const QString& title, const std::vector<float>& values, const QString& unitLabel, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);

    auto* layout = new QVBoxLayout(this);
    auto* stats = statsLabel(this);
    stats->setText(values.empty() ? "This raster has no finite values." : statsSummaryText(computeDescriptiveStats(values), unitLabel));
    layout->addWidget(stats);

    auto* chart = new HistogramChartWidget(this);
    if (!values.empty()) chart->setHistogram(computeHistogram(values), unitLabel);
    layout->addWidget(chart);
    resize(chart->sizeHint().width(), chart->sizeHint().height() + 60);
}

}  // namespace wrftools
