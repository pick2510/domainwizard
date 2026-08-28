#include "wrftools/point_inspector_dialog.hpp"
#include "wrftools/chart_widget.hpp"
#include "wrftools/stats.hpp"

#include <QLabel>
#include <QVBoxLayout>

namespace wrftools {
namespace {
QString statsSummaryText(const DescriptiveStats& stats, const QString& unitLabel) {
    const QString unit = unitLabel.isEmpty() ? QString() : " " + unitLabel;
    return QString("Count: %1    Min: %2%6    Max: %3%6    Mean: %4%6    Median: %7%6    Std Dev: %5%6")
        .arg(stats.count)
        .arg(stats.minimum, 0, 'g', 6)
        .arg(stats.maximum, 0, 'g', 6)
        .arg(stats.mean, 0, 'g', 6)
        .arg(stats.stddev, 0, 'g', 6)
        .arg(unit)
        .arg(stats.median, 0, 'g', 6);
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
