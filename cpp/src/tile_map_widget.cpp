#include "wrftools/tile_map_widget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace wrftools {
TileMapWidget::TileMapWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 200);
    setMouseTracking(true);
    const auto preferred = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/wrftools/tiles";
    cacheDirectory_ = preferred;
    if (!QDir().mkpath(cacheDirectory_)) {
        cacheDirectory_ = QDir::tempPath() + "/wrftools/tiles";
        QDir().mkpath(cacheDirectory_);
    }
}

void TileMapWidget::setCenter(double longitude, double latitude, int zoom) {
    longitude_ = std::clamp(longitude, -180.0, 180.0);
    latitude_ = std::clamp(latitude, -85.05112878, 85.05112878);
    zoom_ = std::clamp(zoom, 0, 19);
    update();
}
void TileMapWidget::zoomToBounds(LonLat southWest, LonLat northEast) {
    const int availableWidth = std::max(1, width() - 40), availableHeight = std::max(1, height() - 40);
    int selectedZoom = 0;
    for (int zoom = 19; zoom >= 0; --zoom) {
        const auto lower = worldPixel(southWest.longitude, southWest.latitude, zoom);
        const auto upper = worldPixel(northEast.longitude, northEast.latitude, zoom);
        if (std::abs(upper.x() - lower.x()) <= availableWidth && std::abs(upper.y() - lower.y()) <= availableHeight) { selectedZoom = zoom; break; }
    }
    setCenter((southWest.longitude + northEast.longitude) / 2.0, (southWest.latitude + northEast.latitude) / 2.0, selectedZoom);
}

void TileMapWidget::setVectorOverlays(std::vector<VectorOverlay> overlays) {
    vectorOverlays_ = std::move(overlays);
    update();
}
void TileMapWidget::setRasterOverlays(std::vector<RasterOverlay> overlays) {
    rasterOverlays_ = std::move(overlays);
    update();
}
void TileMapWidget::setLegend(QPixmap legend) { legend_ = std::move(legend); update(); }

bool TileMapWidget::exportImage(const QString& path) { return grab().save(path); }

QPointF TileMapWidget::worldPixel() const { return worldPixel(longitude_, latitude_, zoom_); }

QPointF TileMapWidget::worldPixel(double longitude, double latitude, int zoom) {
    constexpr double pi = 3.14159265358979323846;
    const auto scale = 256.0 * static_cast<double>(1 << zoom);
    const auto x = (longitude + 180.0) / 360.0 * scale;
    const auto latitudeRadians = std::clamp(latitude, -85.05112878, 85.05112878) * pi / 180.0;
    const auto y = (1.0 - std::asinh(std::tan(latitudeRadians)) / pi) / 2.0 * scale;
    return {x, y};
}

QPointF TileMapWidget::lonLat(QPointF world, int zoom) {
    constexpr double pi = 3.14159265358979323846;
    const auto scale = 256.0 * static_cast<double>(1 << zoom);
    const double longitude = world.x() / scale * 360.0 - 180.0;
    const double latitude = std::atan(std::sinh(pi * (1.0 - 2.0 * world.y() / scale))) * 180.0 / pi;
    return {longitude, latitude};
}

QString TileMapWidget::tileKey(int x, int y, int zoom) const { return QString::number(zoom) + '_' + QString::number(x) + '_' + QString::number(y); }

void TileMapWidget::ensureTile(int x, int y, int zoom) {
    const int count = 1 << zoom;
    x = (x % count + count) % count;
    if (y < 0 || y >= count) return;
    const auto key = tileKey(x, y, zoom);
    if (tiles_.contains(key) || pending_.contains(key)) return;
    const auto path = cacheDirectory_ + '/' + key + ".png";
    QPixmap cached;
    if (cached.load(path)) { tiles_.insert(key, cached); return; }
    pending_.insert(key);
    QNetworkRequest request(QUrl(QString("https://tile.openstreetmap.org/%1/%2/%3.png").arg(zoom).arg(x).arg(y)));
    request.setRawHeader("User-Agent", "WRF-Tools/0.1 (native Qt client)");
    auto* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, path] {
        pending_.remove(key);
        if (reply->error() == QNetworkReply::NoError) {
            QPixmap pixmap;
            if (pixmap.loadFromData(reply->readAll())) {
                tiles_.insert(key, pixmap);
                QSaveFile file(path);
                if (file.open(QIODevice::WriteOnly)) { pixmap.save(&file, "PNG"); file.commit(); }
                update();
            }
        }
        reply->deleteLater();
    });
}

void TileMapWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(232, 238, 242));
    const auto center = worldPixel();
    const auto topLeft = center - QPointF(width() / 2.0, height() / 2.0);
    const int firstX = static_cast<int>(std::floor(topLeft.x() / 256.0));
    const int firstY = static_cast<int>(std::floor(topLeft.y() / 256.0));
    const int lastX = static_cast<int>(std::floor((topLeft.x() + width()) / 256.0));
    const int lastY = static_cast<int>(std::floor((topLeft.y() + height()) / 256.0));
    const int count = 1 << zoom_;
    for (int y = firstY; y <= lastY; ++y) for (int x = firstX; x <= lastX; ++x) {
        ensureTile(x, y, zoom_);
        const int wrappedX = (x % count + count) % count;
        const auto key = tileKey(wrappedX, y, zoom_);
        const QRectF target(x * 256.0 - topLeft.x(), y * 256.0 - topLeft.y(), 256, 256);
        if (const auto tile = tiles_.find(key); tile != tiles_.end()) painter.drawPixmap(target, tile.value(), tile.value().rect());
    }
    for (const auto& overlay : rasterOverlays_) {
        if (overlay.image.isNull()) continue;
        const auto northWest = worldPixel(overlay.southWest.longitude, overlay.northEast.latitude, zoom_) - topLeft;
        const auto southEast = worldPixel(overlay.northEast.longitude, overlay.southWest.latitude, zoom_) - topLeft;
        painter.save();
        painter.setOpacity(std::clamp(overlay.opacity, 0.0, 1.0));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, overlay.smooth);
        painter.drawImage(QRectF(northWest, southEast), overlay.image);
        painter.restore();
    }
    for (const auto& overlay : vectorOverlays_) {
        if (overlay.points.size() < 2) continue;
        QPainterPath path;
        for (std::size_t index = 0; index < overlay.points.size(); ++index) {
            const auto projected = worldPixel(overlay.points[index].longitude, overlay.points[index].latitude, zoom_) - topLeft;
            if (index == 0) path.moveTo(projected); else path.lineTo(projected);
        }
        painter.setPen(QPen(overlay.color, overlay.width));
        painter.drawPath(path);
    }
    if (!legend_.isNull()) {
        const QPointF position = legendPosition_.value_or(QPointF(width() - legend_.width() - 12, 12));
        legendRect_ = QRectF(position, legend_.size());
        painter.drawPixmap(position, legend_);
    } else legendRect_ = {};
    painter.fillRect(QRect(0, 0, 155, 26), QColor(255, 255, 255, 190));
    painter.setPen(QColor(30, 41, 59));
    painter.drawText(QRect(6, 3, 145, 20), QString("%1°, %2°  z%3").arg(longitude_, 0, 'f', 3).arg(latitude_, 0, 'f', 3).arg(zoom_));
    painter.setPen(QColor(100, 116, 139));
    painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignBottom | Qt::AlignLeft,
        "© OpenStreetMap contributors");
}

void TileMapWidget::wheelEvent(QWheelEvent* event) {
    setCenter(longitude_, latitude_, zoom_ + (event->angleDelta().y() > 0 ? 1 : -1));
    event->accept();
}
void TileMapWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (legendRect_.contains(event->position())) {
        draggingLegend_ = true;
        legendOffset_ = event->position() - legendRect_.topLeft();
    } else { dragging_ = true; dragStart_ = event->position(); }
}
void TileMapWidget::mouseMoveEvent(QMouseEvent* event) {
    if (draggingLegend_) {
        legendPosition_ = event->position() - legendOffset_;
        update();
        return;
    }
    if (!dragging_) return;
    const auto delta = event->position() - dragStart_;
    setCenter(lonLat(worldPixel() - delta, zoom_).x(), lonLat(worldPixel() - delta, zoom_).y(), zoom_);
    dragStart_ = event->position();
}
void TileMapWidget::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; draggingLegend_ = false; }
}  // namespace wrftools
