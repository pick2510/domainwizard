#include "wrftools/colorbar.hpp"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>

namespace wrftools {
QPixmap buildColorbar(const std::string& title, float minimum, float maximum, const ColorLut& lut, int tickCount) {
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
        painter.drawText(margin + barWidth + 9, y + 5, QString::number(minimum + static_cast<float>(fraction) * (maximum - minimum), 'g', 4));
    }
    return image;
}
}  // namespace wrftools
