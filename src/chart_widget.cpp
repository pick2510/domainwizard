#include "wrftools/chart_widget.hpp"
#include "wrftools/colorbar.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wrftools {
namespace {
constexpr int kMargin = 44;   // left/bottom axis + label space
constexpr int kTopMargin = 16;
constexpr int kRightMargin = 16;
constexpr int kMaxXTicks = 8;  // more than this and adjacent labels overlap in the dialog's default width

QString tickLabel(double value) { return QString::fromStdString(formatColorbarTick(value)); }
}  // namespace

LineChartWidget::LineChartWidget(QWidget* parent) : QWidget(parent) {}

void LineChartWidget::setSeries(std::vector<QString> labels, std::vector<std::optional<float>> values, QString yAxisLabel) {
    labels_ = std::move(labels);
    values_ = std::move(values);
    yAxisLabel_ = std::move(yAxisLabel);
    update();
}

void LineChartWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Qt::white);
    if (values_.empty()) return;

    float minimum = std::numeric_limits<float>::infinity(), maximum = -std::numeric_limits<float>::infinity();
    for (const auto& value : values_) if (value) { minimum = std::min(minimum, *value); maximum = std::max(maximum, *value); }
    if (!std::isfinite(minimum)) { painter.drawText(rect(), Qt::AlignCenter, "No data at this point."); return; }
    if (minimum == maximum) { minimum -= 1.0f; maximum += 1.0f; }  // avoid a zero-height plot area for a constant series
    const float padding = (maximum - minimum) * 0.08f;
    minimum -= padding; maximum += padding;

    const QRectF plot(kMargin, kTopMargin, width() - kMargin - kRightMargin, height() - kTopMargin - kMargin);
    painter.setPen(QPen(Qt::black, 1));
    painter.drawRect(plot);

    // Y axis ticks/gridlines.
    constexpr int yTicks = 5;
    painter.setPen(QPen(QColor(220, 220, 220), 1));
    for (int i = 0; i <= yTicks; ++i) {
        const double y = plot.bottom() - plot.height() * i / yTicks;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    painter.setPen(Qt::black);
    for (int i = 0; i <= yTicks; ++i) {
        const double y = plot.bottom() - plot.height() * i / yTicks;
        const double value = minimum + (maximum - minimum) * i / yTicks;
        painter.drawText(QRectF(0, y - 8, kMargin - 6, 16), Qt::AlignRight | Qt::AlignVCenter, tickLabel(value));
    }
    if (!yAxisLabel_.isEmpty()) {
        painter.save();
        painter.translate(12, plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-plot.height() / 2, -14, plot.height(), 14), Qt::AlignCenter, yAxisLabel_);
        painter.restore();
    }

    // X axis labels - thinned to at most kMaxXTicks so they don't overlap
    // for a long series.
    const int count = static_cast<int>(values_.size());
    const int step = std::max(1, (count + kMaxXTicks - 1) / kMaxXTicks);
    for (int i = 0; i < count; i += step) {
        const double x = count > 1 ? plot.left() + plot.width() * i / (count - 1) : plot.center().x();
        painter.drawText(QRectF(x - 40, plot.bottom() + 4, 80, 16), Qt::AlignCenter, labels_[static_cast<std::size_t>(i)]);
    }

    // The line itself - breaks (no segment drawn) across a nullopt gap.
    painter.setPen(QPen(QColor(31, 119, 180), 2));
    std::optional<QPointF> previous;
    for (int i = 0; i < count; ++i) {
        if (!values_[static_cast<std::size_t>(i)]) { previous.reset(); continue; }
        const double x = count > 1 ? plot.left() + plot.width() * i / (count - 1) : plot.center().x();
        const double y = plot.bottom() - plot.height() * (*values_[static_cast<std::size_t>(i)] - minimum) / (maximum - minimum);
        const QPointF point(x, y);
        if (previous) painter.drawLine(*previous, point);
        painter.drawEllipse(point, 2.0, 2.0);
        previous = point;
    }
}

HistogramChartWidget::HistogramChartWidget(QWidget* parent) : QWidget(parent) {}

void HistogramChartWidget::setHistogram(Histogram histogram, QString xAxisLabel) {
    histogram_ = std::move(histogram);
    xAxisLabel_ = std::move(xAxisLabel);
    update();
}

void HistogramChartWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    if (histogram_.binCounts.empty()) return;

    const std::size_t maxCount = *std::max_element(histogram_.binCounts.begin(), histogram_.binCounts.end());
    const QRectF plot(kMargin, kTopMargin, width() - kMargin - kRightMargin, height() - kTopMargin - kMargin);
    painter.setPen(QPen(Qt::black, 1));
    painter.drawRect(plot);

    constexpr int yTicks = 5;
    painter.setPen(QPen(QColor(220, 220, 220), 1));
    for (int i = 0; i <= yTicks; ++i) {
        const double y = plot.bottom() - plot.height() * i / yTicks;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    painter.setPen(Qt::black);
    for (int i = 0; i <= yTicks; ++i) {
        const double y = plot.bottom() - plot.height() * i / yTicks;
        const auto value = static_cast<double>(maxCount) * i / yTicks;
        painter.drawText(QRectF(0, y - 8, kMargin - 6, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(value, 'f', 0));
    }

    const auto binCount = static_cast<int>(histogram_.binCounts.size());
    const double barSlot = plot.width() / binCount;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(31, 119, 180));
    for (int i = 0; i < binCount; ++i) {
        const double barHeight = maxCount > 0 ? plot.height() * static_cast<double>(histogram_.binCounts[static_cast<std::size_t>(i)]) / static_cast<double>(maxCount) : 0.0;
        painter.drawRect(QRectF(plot.left() + i * barSlot + 1, plot.bottom() - barHeight, barSlot - 2, barHeight));
    }

    painter.setPen(Qt::black);
    const int step = std::max(1, (binCount + kMaxXTicks - 1) / kMaxXTicks);
    for (int i = 0; i <= binCount; i += step) {
        const double x = plot.left() + i * barSlot;
        const double value = histogram_.binStart + i * histogram_.binWidth;
        painter.drawText(QRectF(x - 40, plot.bottom() + 4, 80, 16), Qt::AlignCenter, tickLabel(value));
    }
    if (!xAxisLabel_.isEmpty()) painter.drawText(QRectF(plot.left(), plot.bottom() + 20, plot.width(), 16), Qt::AlignCenter, xAxisLabel_);
}

}  // namespace wrftools
