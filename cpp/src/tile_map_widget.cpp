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
namespace {
// Web Mercator's half-circumference in metres (pi * 6378137, the EPSG:3857
// convention - not the WRF sphere used elsewhere in this app). Matches
// wrftools.tilemap.MERC_HALF.
constexpr double kMercHalf = 20037508.342789244;
}

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
        const auto lower = worldPixel(southWest.lon, southWest.lat, zoom);
        const auto upper = worldPixel(northEast.lon, northEast.lat, zoom);
        if (std::abs(upper.x() - lower.x()) <= availableWidth && std::abs(upper.y() - lower.y()) <= availableHeight) { selectedZoom = zoom; break; }
    }
    setCenter((southWest.lon + northEast.lon) / 2.0, (southWest.lat + northEast.lat) / 2.0, selectedZoom);
}

std::pair<LonLat, LonLat> TileMapWidget::currentViewBounds() const {
    const auto center = worldPixel();
    const auto topLeft = lonLat(center - QPointF(width() / 2.0, height() / 2.0), zoom_);
    const auto bottomRight = lonLat(center + QPointF(width() / 2.0, height() / 2.0), zoom_);
    return {{topLeft.x(), bottomRight.y()}, {bottomRight.x(), topLeft.y()}};
}

void TileMapWidget::setVectorOverlayGroup(const QString& name, std::vector<VectorOverlay> overlays, int z) {
    vectorGroups_[name] = {z, std::move(overlays)};
    update();
}
void TileMapWidget::clearVectorOverlayGroup(const QString& name) { vectorGroups_.remove(name); update(); }
void TileMapWidget::setRasterOverlayGroup(const QString& name, std::vector<RasterOverlay> overlays, int z) {
    rasterGroups_[name] = {z, std::move(overlays)};
    update();
}
void TileMapWidget::clearRasterOverlayGroup(const QString& name) { rasterGroups_.remove(name); update(); }
void TileMapWidget::setLegend(QPixmap legend) { legend_ = std::move(legend); update(); }
void TileMapWidget::setInfoText(const QString& text) { infoText_ = text; update(); }

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

QPointF TileMapWidget::mercatorWorldPixel(double x, double y, int zoom) {
    const auto scale = 256.0 * static_cast<double>(1 << zoom);
    return {(x + kMercHalf) / (2 * kMercHalf) * scale, (kMercHalf - y) / (2 * kMercHalf) * scale};
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

QRectF TileMapWidget::movableRect(QSizeF size, const std::optional<QPointF>& position, bool topRight) const {
    constexpr double margin = 10.0;
    QPointF origin = position.value_or(topRight ? QPointF(width() - size.width() - margin, margin) : QPointF(margin, margin));
    origin.setX(std::clamp(origin.x(), 0.0, std::max(0.0, width() - size.width())));
    origin.setY(std::clamp(origin.y(), 0.0, std::max(0.0, height() - size.height())));
    return {origin, size};
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

    // Raster groups paint under vector groups; within each kind, groups
    // paint low-z-first, mirroring wrftools.tilemap's Z_RASTER < Z_VECTOR
    // fixed ordering (see tile_map_widget.hpp's kRasterOverlayZ/kVectorOverlayZ).
    auto rasterKeys = rasterGroups_.keys();
    std::sort(rasterKeys.begin(), rasterKeys.end(), [this](const QString& a, const QString& b) { return rasterGroups_[a].z < rasterGroups_[b].z; });
    for (const auto& key : rasterKeys) for (const auto& overlay : rasterGroups_[key].overlays) {
        if (overlay.image.isNull()) continue;
        const auto northWest = mercatorWorldPixel(overlay.bounds3857.minX, overlay.bounds3857.maxY, zoom_) - topLeft;
        const auto southEast = mercatorWorldPixel(overlay.bounds3857.maxX, overlay.bounds3857.minY, zoom_) - topLeft;
        painter.save();
        painter.setOpacity(std::clamp(overlay.opacity, 0.0, 1.0));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, overlay.smooth);
        painter.drawImage(QRectF(northWest, southEast), overlay.image);
        painter.restore();
    }

    auto vectorKeys = vectorGroups_.keys();
    std::sort(vectorKeys.begin(), vectorKeys.end(), [this](const QString& a, const QString& b) { return vectorGroups_[a].z < vectorGroups_[b].z; });
    for (const auto& key : vectorKeys) for (const auto& overlay : vectorGroups_[key].overlays) {
        if (overlay.points.size() < 2) continue;
        QPainterPath path;
        for (std::size_t index = 0; index < overlay.points.size(); ++index) {
            const auto projected = worldPixel(overlay.points[index].lon, overlay.points[index].lat, zoom_) - topLeft;
            if (index == 0) path.moveTo(projected); else path.lineTo(projected);
        }
        if (overlay.closed) path.closeSubpath();
        painter.setPen(QPen(overlay.color, overlay.width));
        painter.drawPath(path);
    }

    if (!legend_.isNull()) {
        legendRect_ = movableRect(legend_.size(), legendPosition_, /*topRight=*/true);
        painter.drawPixmap(legendRect_.topLeft(), legend_);
    } else legendRect_ = {};

    if (!infoText_.isEmpty()) {
        constexpr double padding = 6.0;
        const auto textRect = painter.fontMetrics().boundingRect(infoText_);
        const QSizeF boxSize(textRect.width() + 2 * padding, textRect.height() + 2 * padding);
        infoRect_ = movableRect(boxSize, infoPosition_, /*topRight=*/false);
        painter.fillRect(infoRect_, QColor(255, 255, 255, 220));
        painter.setPen(QColor(120, 120, 120));
        painter.drawRect(infoRect_);
        painter.setPen(Qt::black);
        painter.drawText(infoRect_, Qt::AlignLeft | Qt::AlignVCenter, infoText_);
    } else infoRect_ = {};

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
        dragTarget_ = "legend";
        dragOffset_ = event->position() - legendRect_.topLeft();
    } else if (infoRect_.contains(event->position())) {
        dragTarget_ = "info";
        dragOffset_ = event->position() - infoRect_.topLeft();
    } else { dragging_ = true; dragStart_ = event->position(); }
}
void TileMapWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragTarget_ == "legend") { legendPosition_ = event->position() - dragOffset_; update(); return; }
    if (dragTarget_ == "info") { infoPosition_ = event->position() - dragOffset_; update(); return; }
    if (!dragging_) return;
    const auto delta = event->position() - dragStart_;
    setCenter(lonLat(worldPixel() - delta, zoom_).x(), lonLat(worldPixel() - delta, zoom_).y(), zoom_);
    dragStart_ = event->position();
}
void TileMapWidget::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; dragTarget_.clear(); }
}  // namespace wrftools
