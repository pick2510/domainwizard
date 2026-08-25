#include "wrftools/colorbar.hpp"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cstdio>

namespace wrftools {
namespace {
std::string formatTick(double value, const std::string& format, int decimals) {
    char buffer[64];
    if (format == "fixed") std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    else if (format == "scientific") std::snprintf(buffer, sizeof buffer, "%.*e", decimals, value);
    else std::snprintf(buffer, sizeof buffer, "%.3g", value);
    return buffer;
}
}

QPixmap buildColorbar(const std::string& title, float minimum, float maximum, const ColorLut& lut, int tickCount, const std::string& tickFormat, int tickDecimals) {
    tickCount = std::clamp(tickCount, 2, 12);
    constexpr int barWidth = 24, barHeight = 220, margin = 10;
    QPixmap image(180, barHeight + 54);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillRect(image.rect(), QColor(255, 255, 255, 235));
    painter.setPen(Qt::black);
    painter.drawText(QRect(margin, 4, image.width() - 2 * margin, 20), QString::fromStdString(title));
    for (int y = 0; y < barHeight; ++y) {
        const auto& color = lut[255 - y * 255 / (barHeight - 1)];
        painter.setPen(QColor(color[0], color[1], color[2]));
        painter.drawLine(margin, 28 + y, margin + barWidth, 28 + y);
    }
    painter.setPen(Qt::black);
    painter.drawRect(margin, 28, barWidth, barHeight);
    for (int i = 0; i < tickCount; ++i) {
        const double fraction = static_cast<double>(i) / (tickCount - 1);
        const int y = 28 + static_cast<int>((1.0 - fraction) * barHeight);
        painter.drawLine(margin + barWidth, y, margin + barWidth + 5, y);
        const double value = minimum + fraction * (maximum - minimum);
        painter.drawText(margin + barWidth + 9, y + 5, QString::fromStdString(formatTick(value, tickFormat, tickDecimals)));
    }
    return image;
}

QPixmap buildCategoricalLegend(const ColorLut& lut, const std::map<int, std::string>& labels, const std::vector<int>& present, const std::string& title) {
    constexpr int margin = 10, titleHeight = 20, swatchSize = 12, rowHeight = 16, maxRows = 20;
    const int rowCount = std::min<int>(static_cast<int>(present.size()), maxRows);
    const int extra = static_cast<int>(present.size()) - rowCount;
    const int totalRows = rowCount + (extra > 0 ? 1 : 0);

    QFont labelFont; labelFont.setPointSize(8);
    QFontMetrics metrics(labelFont);
    int maxLabelWidth = 0;
    for (int i = 0; i < rowCount; ++i) {
        const int value = present[static_cast<std::size_t>(i)];
        const auto found = labels.find(value);
        const auto text = found != labels.end() ? found->second : ("Category " + std::to_string(value));
        maxLabelWidth = std::max(maxLabelWidth, metrics.horizontalAdvance(QString::fromStdString(text)));
    }
    if (extra > 0) maxLabelWidth = std::max(maxLabelWidth, metrics.horizontalAdvance(QString("+%1 more").arg(extra)));

    const int width = 2 * margin + swatchSize + 6 + maxLabelWidth;
    const int height = 2 * margin + titleHeight + totalRows * rowHeight;
    QPixmap image(std::max(width, 2 * margin), std::max(height, margin + titleHeight));
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillRect(image.rect(), QColor(255, 255, 255, 235));
    painter.setPen(Qt::black);
    painter.drawText(QRect(margin, 4, image.width() - 2 * margin, titleHeight), QString::fromStdString(title));
    painter.setFont(labelFont);

    for (int i = 0; i < rowCount; ++i) {
        const int value = present[static_cast<std::size_t>(i)];
        const int rowTop = margin + titleHeight + i * rowHeight;
        const auto& color = lut[static_cast<std::size_t>(std::clamp(value, 0, 255))];
        painter.setPen(QColor(90, 90, 90));
        painter.setBrush(QColor(color[0], color[1], color[2]));
        painter.drawRect(margin, rowTop + (rowHeight - swatchSize) / 2, swatchSize, swatchSize);
        const auto found = labels.find(value);
        const auto text = found != labels.end() ? found->second : ("Category " + std::to_string(value));
        painter.setPen(QColor(20, 20, 20));
        painter.drawText(margin + swatchSize + 6, rowTop + metrics.ascent() + (rowHeight - metrics.height()) / 2, QString::fromStdString(text));
    }
    if (extra > 0) {
        const int rowTop = margin + titleHeight + rowCount * rowHeight;
        painter.setPen(QColor(90, 90, 90));
        painter.drawText(margin + swatchSize + 6, rowTop + metrics.ascent() + (rowHeight - metrics.height()) / 2, QString("+%1 more").arg(extra));
    }
    return image;
}
}  // namespace wrftools
