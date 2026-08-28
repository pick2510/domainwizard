#include "wrftools/tile_map_widget.hpp"

#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QResizeEvent>
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

std::vector<TileProvider> builtinTileProviders() {
    return {
        {"OpenStreetMap Standard", "http://tile.openstreetmap.org/{z}/{x}/{y}.png", "OpenStreetMap contributors, under ODbL", 19, false},
        {"Google Maps", "https://mt1.google.com/vt/lyrs=m&x={x}&y={y}&z={z}", "", 19, false},
        {"Google Satellite", "https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}", "", 19, false},
        {"Google Terrain", "https://mt1.google.com/vt/lyrs=t&x={x}&y={y}&z={z}", "", 19, false},
        {"Google Terrain Hybrid", "https://mt1.google.com/vt/lyrs=p&x={x}&y={y}&z={z}", "", 19, false},
        {"Google Satellite Hybrid", "https://mt1.google.com/vt/lyrs=y&x={x}&y={y}&z={z}", "", 19, false},
        {"Esri Boundaries Places", "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 20, false},
        {"Esri Gray (dark)", "http://services.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 16, false},
        {"Esri Gray (light)", "http://services.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Light_Gray_Base/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 16, false},
        {"Esri Hillshade", "http://services.arcgisonline.com/ArcGIS/rest/services/Elevation/World_Hillshade/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 12, false},
        {"Esri National Geographic", "http://services.arcgisonline.com/ArcGIS/rest/services/NatGeo_World_Map/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 12, false},
        {"Esri Navigation Charts", "http://services.arcgisonline.com/ArcGIS/rest/services/Specialty/World_Navigation_Charts/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 12, false},
        {"Esri Ocean", "https://services.arcgisonline.com/ArcGIS/rest/services/Ocean/World_Ocean_Base/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 10, false},
        {"Esri Physical Map", "https://services.arcgisonline.com/ArcGIS/rest/services/World_Physical_Map/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 10, false},
        {"Esri Satellite", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 17, false},
        {"Esri Shaded Relief", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Shaded_Relief/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 17, false},
        {"Esri Standard", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 17, false},
        {"Esri Terrain", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Terrain_Base/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 13, false},
        {"Esri Transportation", "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Transportation/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 20, false},
        {"Esri Topo World", "http://services.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}", "Requires ArcGIS Online subscription", 20, false},
        {"OpenStreetMap H.O.T.", "http://tile.openstreetmap.fr/hot/{z}/{x}/{y}.png", "OpenStreetMap contributors, under ODbL", 19, false},
        {"OpenTopoMap", "https://tile.opentopomap.org/{z}/{x}/{y}.png", "Kartendaten: © OpenStreetMap-Mitwirkende, SRTM | Kartendarstellung: © OpenTopoMap (CC-BY-SA)", 17, true},
        {"Strava All", "https://heatmap-external-b.strava.com/tiles/all/bluered/{z}/{x}/{y}.png", "OpenStreetMap contributors, under ODbL", 15, false},
        {"Strava Run", "https://heatmap-external-b.strava.com/tiles/run/bluered/{z}/{x}/{y}.png?v=19", "OpenStreetMap contributors, under ODbL", 15, false},
        {"CartoDb Dark Matter", "http://basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png", "Map tiles by CartoDB, under CC BY 3.0. Data by OpenStreetMap, under ODbL.", 20, false},
        {"CartoDb Dark Matter (No Labels)", "http://basemaps.cartocdn.com/dark_nolabels/{z}/{x}/{y}.png", "Map tiles by CartoDB, under CC BY 3.0. Data by OpenStreetMap, under ODbL.", 20, false},
        {"CartoDb Positron", "http://basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png", "Map tiles by CartoDB, under CC BY 3.0. Data by OpenStreetMap, under ODbL.", 20, false},
        {"CartoDb Positron (No Labels)", "http://basemaps.cartocdn.com/light_nolabels/{z}/{x}/{y}.png", "Map tiles by CartoDB, under CC BY 3.0. Data by OpenStreetMap, under ODbL.", 20, false},
        {"Bing VirtualEarth", "http://ecn.t3.tiles.virtualearth.net/tiles/a{q}.jpeg?g=1", "", 19, true},
    };
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

    providers_ = builtinTileProviders();
    providerCombo_ = new QComboBox(this);
    for (const auto& provider : providers_) providerCombo_->addItem(provider.name);
    providerCombo_->setCurrentIndex(currentProviderIndex_);
    // Explicit text/selection colors, not just background - without them,
    // the combo box (and especially its popup list) inherits whatever the
    // active OS/Qt theme uses for text, which on a dark theme can render as
    // white-on-white against this widget's deliberately light background.
    providerCombo_->setStyleSheet(
        "QComboBox { background-color: rgba(255, 255, 255, 220); color: #1e293b; }"
        "QComboBox QAbstractItemView { background-color: #ffffff; color: #1e293b; selection-background-color: #3b82f6; selection-color: #ffffff; }");
    connect(providerCombo_, &QComboBox::currentIndexChanged, this, [this](int index) { setTileProvider(index); });
    repositionOverlayControls();
}

void TileMapWidget::setTileProvider(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= providers_.size() || index == currentProviderIndex_) return;
    currentProviderIndex_ = index;
    if (providerCombo_->currentIndex() != index) providerCombo_->setCurrentIndex(index);
    update();
}

void TileMapWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    repositionOverlayControls();
}

void TileMapWidget::repositionOverlayControls() {
    if (!providerCombo_) return;
    // Below the coordinate/zoom readout drawn in the top-left corner by
    // paintEvent (a 155x26 box starting at the origin) - avoids overlapping
    // it or the movable legend/info boxes, which default to the top-right
    // and top-left corners respectively but start further down.
    constexpr int margin = 6;
    constexpr int readoutHeight = 26;
    providerCombo_->move(margin, readoutHeight + margin);
    providerCombo_->resize(std::min(220, width() - 2 * margin), providerCombo_->sizeHint().height());
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
void TileMapWidget::setHoverValueHandler(HoverValueHandler handler) { hoverValueHandler_ = std::move(handler); }
void TileMapWidget::setShowNorthArrow(bool show) { showNorthArrow_ = show; update(); }

void TileMapWidget::setDraggableVectorOverlayGroup(const QString& groupName) { draggableGroup_ = groupName; }
void TileMapWidget::setOverlayDragHandlers(OverlayDragStartHandler onStart, OverlayDragMoveHandler onMove, OverlayDragEndHandler onEnd) {
    overlayDragStart_ = std::move(onStart);
    overlayDragMove_ = std::move(onMove);
    overlayDragEnd_ = std::move(onEnd);
}
void TileMapWidget::setOverlayResizeHandlers(OverlayResizeStartHandler onStart, OverlayResizeMoveHandler onMove, OverlayResizeEndHandler onEnd) {
    overlayResizeStart_ = std::move(onStart);
    overlayResizeMove_ = std::move(onMove);
    overlayResizeEnd_ = std::move(onEnd);
}

bool TileMapWidget::hitTestOverlayHandle(const QString& groupName, QPointF screenPoint, std::size_t& outOverlayIndex, std::size_t& outHandleIndex) const {
    const auto found = vectorGroups_.find(groupName);
    if (found == vectorGroups_.end()) return false;
    constexpr double hitRadius = 8.0;
    const auto topLeft = viewportTopLeft();
    const auto& overlays = found->overlays;
    for (std::size_t i = overlays.size(); i-- > 0;) {
        const auto& overlay = overlays[i];
        for (std::size_t h = 0; h < overlay.handles.size(); ++h) {
            const auto handlePoint = worldPixel(overlay.handles[h].lon, overlay.handles[h].lat, zoom_) - topLeft;
            const auto delta = handlePoint - screenPoint;
            if (std::hypot(delta.x(), delta.y()) <= hitRadius) { outOverlayIndex = i; outHandleIndex = h; return true; }
        }
    }
    return false;
}

bool TileMapWidget::hitTestOverlay(const QString& groupName, QPointF screenPoint, std::size_t& outIndex) const {
    const auto found = vectorGroups_.find(groupName);
    if (found == vectorGroups_.end()) return false;
    const auto topLeft = viewportTopLeft();
    const auto& overlays = found->overlays;
    // Last-in-the-group first: DomainForm always appends a child after its
    // parent, so a nested domain's (smaller, fully-enclosed) polygon is
    // tested - and wins - before the parent's.
    for (std::size_t i = overlays.size(); i-- > 0;) {
        const auto& overlay = overlays[i];
        if (!overlay.closed || overlay.points.size() < 3) continue;
        bool inside = false;
        for (std::size_t a = 0, b = overlay.points.size() - 1; a < overlay.points.size(); b = a++) {
            const auto pa = worldPixel(overlay.points[a].lon, overlay.points[a].lat, zoom_) - topLeft;
            const auto pb = worldPixel(overlay.points[b].lon, overlay.points[b].lat, zoom_) - topLeft;
            const bool crosses = ((pa.y() > screenPoint.y()) != (pb.y() > screenPoint.y())) &&
                (screenPoint.x() < (pb.x() - pa.x()) * (screenPoint.y() - pa.y()) / (pb.y() - pa.y()) + pa.x());
            if (crosses) inside = !inside;
        }
        if (inside) { outIndex = i; return true; }
    }
    return false;
}

bool TileMapWidget::exportImage(const QString& path) {
    // The provider combo is a real child QWidget, not something paintEvent
    // draws - grab() captures it like any other child, so it has to be
    // hidden for the export and restored afterward rather than just skipped
    // by a paint-time flag.
    const bool wasVisible = providerCombo_->isVisible();
    providerCombo_->setVisible(false);
    // The hover readout is transient, cursor-following UI chrome, not part
    // of the map itself - blanked for the grab and restored right after, so
    // an in-progress hover never ends up baked into a saved image.
    const QString savedHover = hoverText_;
    hoverText_.clear();
    const auto image = grab();
    hoverText_ = savedHover;
    providerCombo_->setVisible(wasVisible);
    return image.save(path);
}

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

QString TileMapWidget::tileKey(int x, int y, int zoom) const {
    const auto& provider = currentTileProvider();
    return provider.name + '_' + QString::number(zoom) + '_' + QString::number(x) + '_' + QString::number(y);
}

QString TileMapWidget::quadKey(int x, int y, int zoom) {
    QString key;
    for (int i = zoom; i > 0; --i) {
        int digit = 0;
        const int mask = 1 << (i - 1);
        if ((x & mask) != 0) digit += 1;
        if ((y & mask) != 0) digit += 2;
        key.append(QChar('0' + digit));
    }
    return key;
}

void TileMapWidget::ensureTile(int x, int y, int zoom) {
    const int count = 1 << zoom;
    x = (x % count + count) % count;
    if (y < 0 || y >= count) return;
    const auto& provider = currentTileProvider();
    if (zoom > provider.maxZoom) return;
    const auto key = tileKey(x, y, zoom);
    if (tiles_.contains(key) || pending_.contains(key)) return;
    const auto path = cacheDirectory_ + '/' + key + ".png";
    QPixmap cached;
    if (cached.load(path)) { tiles_.insert(key, cached); return; }
    pending_.insert(key);
    QString url = provider.url;
    url.replace("{z}", QString::number(zoom)).replace("{x}", QString::number(x)).replace("{y}", QString::number(y)).replace("{q}", quadKey(x, y, zoom));
    QNetworkRequest request{QUrl(url)};
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

QPainterPath TileMapWidget::overlayPath(const VectorOverlay& overlay, QPointF topLeft) const {
    QPainterPath path;
    for (std::size_t index = 0; index < overlay.points.size(); ++index) {
        const auto projected = worldPixel(overlay.points[index].lon, overlay.points[index].lat, zoom_) - topLeft;
        if (index == 0) path.moveTo(projected); else path.lineTo(projected);
    }
    if (overlay.closed) path.closeSubpath();
    return path;
}

QPointF TileMapWidget::viewportTopLeft() const {
    const auto center = worldPixel();
    return center - QPointF(width() / 2.0, height() / 2.0);
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
    const auto topLeft = viewportTopLeft();
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
        const auto path = overlayPath(overlay, topLeft);
        if (overlay.closed && overlay.fill.alpha() > 0) painter.fillPath(path, overlay.fill);
        painter.setPen(QPen(overlay.color, overlay.width));
        painter.drawPath(path);
    }

    // While an overlay drag is in progress (see setOverlayDragHandlers),
    // redraw just that one polygon with a high-contrast white-halo +
    // black-dashed outline on top of everything else - the plain colored
    // outline used above is easy to lose track of against similarly
    // colored basemap tiles/neighboring domains while dragging, making
    // precise placement hard to judge.
    if (dragTarget_ == "overlay" && vectorGroups_.contains(draggableGroup_)) {
        const auto& overlays = vectorGroups_[draggableGroup_].overlays;
        if (draggedOverlayIndex_ < overlays.size()) {
            const auto& dragged = overlays[draggedOverlayIndex_];
            if (dragged.points.size() >= 2) {
                const auto path = overlayPath(dragged, topLeft);
                painter.setPen(QPen(Qt::white, dragged.width + 4));
                painter.drawPath(path);
                QPen outline(Qt::black, dragged.width + 1.5);
                outline.setDashPattern({4, 3});
                painter.setPen(outline);
                painter.drawPath(path);
            }
        }
    }

    // Corner resize handles - drawn for every overlay in the draggable
    // group whenever one exists (an always-visible affordance, matching
    // the legend's own resize handle), not just the one currently being
    // resized.
    if (!draggableGroup_.isEmpty() && vectorGroups_.contains(draggableGroup_)) {
        constexpr double half = 4.0;
        painter.setPen(QPen(Qt::black, 1));
        painter.setBrush(Qt::white);
        for (const auto& overlay : vectorGroups_[draggableGroup_].overlays)
            for (const auto& handle : overlay.handles) {
                const auto p = worldPixel(handle.lon, handle.lat, zoom_) - topLeft;
                painter.drawRect(QRectF(p.x() - half, p.y() - half, half * 2, half * 2));
            }
    }

    if (!legend_.isNull()) {
        const QSizeF scaledSize = QSizeF(legend_.size()) * legendScale_;
        legendRect_ = movableRect(scaledSize, legendPosition_, /*topRight=*/true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(legendRect_, legend_, QRectF(QPointF(0, 0), legend_.size()));
        constexpr double handleSize = 11.0;
        legendResizeHandleRect_ = QRectF(legendRect_.right() - handleSize, legendRect_.bottom() - handleSize, handleSize, handleSize);
        painter.fillRect(legendResizeHandleRect_, QColor(90, 90, 90, 200));
    } else { legendRect_ = {}; legendResizeHandleRect_ = {}; }

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

    // North arrow - fixed in the bottom-right corner, away from the legend
    // (top-right), info/hover boxes (left side) and the coordinate readout
    // (top-left). Straight up always points north here: the basemap is
    // never rotated.
    if (showNorthArrow_) {
        painter.save();
        constexpr double margin = 14.0, arrowWidth = 16.0, arrowHeight = 30.0, labelHeight = 16.0;
        const QPointF baseLeft(width() - margin - arrowWidth, height() - margin - labelHeight);
        const QPointF baseRight(width() - margin, height() - margin - labelHeight);
        const QPointF baseMid((baseLeft.x() + baseRight.x()) / 2.0, baseLeft.y() - 6.0);
        const QPointF tip((baseLeft.x() + baseRight.x()) / 2.0, baseLeft.y() - arrowHeight);
        QPainterPath arrow;
        arrow.moveTo(tip);
        arrow.lineTo(baseRight);
        arrow.lineTo(baseMid);
        arrow.lineTo(baseLeft);
        arrow.closeSubpath();
        painter.setPen(QPen(Qt::black, 1));
        painter.setBrush(Qt::white);
        painter.drawPath(arrow);
        QFont font = painter.font();
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(QRectF(width() - margin - arrowWidth, height() - margin - labelHeight, arrowWidth, labelHeight), Qt::AlignHCenter | Qt::AlignVCenter, "N");
        painter.restore();
    }

    // Mouse-hover readout - bottom-left corner, not movable/draggable
    // (unlike the legend/info boxes) since it only makes sense right where
    // the cursor already is. Cleared on exportImage() (see there) and on
    // leaveEvent, so it never lingers once the mouse leaves the widget.
    if (!hoverText_.isEmpty()) {
        constexpr double padding = 6.0;
        const auto textRect = painter.fontMetrics().boundingRect(hoverText_);
        const QSizeF boxSize(textRect.width() + 2 * padding, textRect.height() + 2 * padding);
        const QRectF hoverBox(10.0, height() - boxSize.height() - 10.0, boxSize.width(), boxSize.height());
        painter.fillRect(hoverBox, QColor(255, 255, 255, 220));
        painter.setPen(QColor(120, 120, 120));
        painter.drawRect(hoverBox);
        painter.setPen(Qt::black);
        painter.drawText(hoverBox, Qt::AlignCenter, hoverText_);
    }

    painter.fillRect(QRect(0, 0, 155, 26), QColor(255, 255, 255, 190));
    painter.setPen(QColor(30, 41, 59));
    painter.drawText(QRect(6, 3, 145, 20), QString("%1°, %2°  z%3").arg(longitude_, 0, 'f', 3).arg(latitude_, 0, 'f', 3).arg(zoom_));
}

void TileMapWidget::wheelEvent(QWheelEvent* event) {
    setCenter(longitude_, latitude_, zoom_ + (event->angleDelta().y() > 0 ? 1 : -1));
    event->accept();
}
void TileMapWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (legendResizeHandleRect_.contains(event->position())) {
        dragTarget_ = "legend-resize";
        legendResizeOrigin_ = legendRect_.topLeft();
    } else if (legendRect_.contains(event->position())) {
        dragTarget_ = "legend";
        dragOffset_ = event->position() - legendRect_.topLeft();
    } else if (infoRect_.contains(event->position())) {
        dragTarget_ = "info";
        dragOffset_ = event->position() - infoRect_.topLeft();
    } else if (!draggableGroup_.isEmpty() && hitTestOverlayHandle(draggableGroup_, event->position(), resizedOverlayIndex_, resizedHandleIndex_)) {
        // Checked before the body hit test below: a handle sits right on
        // the polygon's own edge, which would otherwise also register as
        // "inside" it.
        dragTarget_ = "overlay-resize";
        const auto pressLonLat = lonLat(event->position() + viewportTopLeft(), zoom_);
        if (overlayResizeStart_) overlayResizeStart_(resizedOverlayIndex_, resizedHandleIndex_, {pressLonLat.x(), pressLonLat.y()});
    } else if (!draggableGroup_.isEmpty() && hitTestOverlay(draggableGroup_, event->position(), draggedOverlayIndex_)) {
        dragTarget_ = "overlay";
        const auto pressLonLat = lonLat(event->position() + viewportTopLeft(), zoom_);
        if (overlayDragStart_) overlayDragStart_(draggedOverlayIndex_, {pressLonLat.x(), pressLonLat.y()});
    } else { dragging_ = true; dragStart_ = event->position(); }
}
void TileMapWidget::mouseMoveEvent(QMouseEvent* event) {
    // Recomputes the hover readout from the *current* longitude_/latitude_/
    // zoom_ - called once per branch below, always after that branch has
    // finished mutating those (the panning branch moves the center via
    // setCenter() before calling this), so the readout never lags a frame
    // behind a live pan the way computing it up front unconditionally would.
    auto updateHover = [this, event] {
        const auto here = lonLat(event->position() + viewportTopLeft(), zoom_);
        QString text = QString("%1°, %2°").arg(here.x(), 0, 'f', 4).arg(here.y(), 0, 'f', 4);
        if (hoverValueHandler_) {
            if (const auto value = hoverValueHandler_(LonLat{here.x(), here.y()})) text = *value + "   " + text;
        }
        hoverText_ = text;
    };
    if (dragTarget_ == "legend-resize") {
        if (const double baseWidth = legend_.width(); baseWidth > 0) {
            const double rawWidth = event->position().x() - legendResizeOrigin_.x();
            legendScale_ = std::clamp(rawWidth / baseWidth, 0.4, 3.0);
        }
        updateHover();
        update();
        return;
    }
    if (dragTarget_ == "legend") { legendPosition_ = event->position() - dragOffset_; updateHover(); update(); return; }
    if (dragTarget_ == "info") { infoPosition_ = event->position() - dragOffset_; updateHover(); update(); return; }
    if (dragTarget_ == "overlay") {
        // No update() here: the handler is expected to call
        // setVectorOverlayGroup() with the recomputed outlines, which
        // triggers its own repaint.
        updateHover();
        if (overlayDragMove_) {
            const auto currentLonLat = lonLat(event->position() + viewportTopLeft(), zoom_);
            overlayDragMove_(draggedOverlayIndex_, {currentLonLat.x(), currentLonLat.y()});
        }
        return;
    }
    if (dragTarget_ == "overlay-resize") {
        updateHover();
        if (overlayResizeMove_) {
            const auto currentLonLat = lonLat(event->position() + viewportTopLeft(), zoom_);
            overlayResizeMove_(resizedOverlayIndex_, resizedHandleIndex_, {currentLonLat.x(), currentLonLat.y()});
        }
        return;
    }
    if (!dragging_) { updateHover(); update(); return; }  // idle hover
    const auto delta = event->position() - dragStart_;
    setCenter(lonLat(worldPixel() - delta, zoom_).x(), lonLat(worldPixel() - delta, zoom_).y(), zoom_);
    updateHover();  // after the center actually moved, so the readout matches the new viewport
    dragStart_ = event->position();
}
void TileMapWidget::mouseReleaseEvent(QMouseEvent*) {
    if (dragTarget_ == "overlay" && overlayDragEnd_) overlayDragEnd_();
    if (dragTarget_ == "overlay-resize" && overlayResizeEnd_) overlayResizeEnd_();
    dragging_ = false;
    dragTarget_.clear();
}
void TileMapWidget::leaveEvent(QEvent*) {
    hoverText_.clear();
    update();
}
}  // namespace wrftools
