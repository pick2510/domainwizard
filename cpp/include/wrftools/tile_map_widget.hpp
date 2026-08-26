#pragma once

#include "wrftools/crs.hpp"

#include <optional>
#include <utility>
#include <vector>

#include <QNetworkAccessManager>
#include <QHash>
#include <QPointF>
#include <QSet>
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QRectF>
#include <QWidget>

namespace wrftools {
// LonLat is defined in crs.hpp (lon/lat fields) and reused here.
struct VectorOverlay { std::vector<LonLat> points; QColor color{Qt::red}; double width{2.0}; bool closed{false}; };
// image is already reprojected to EPSG:3857 (see warp.hpp) - bounds3857 is
// therefore a pure scale+translate placement, never a stretch/skew of the
// source grid, unlike the old lon/lat-corner placement this replaced.
struct RasterOverlay { QImage image; Bounds2D bounds3857; double opacity{1.0}; bool smooth{true}; };

// Default z for a raster overlay group and a vector overlay group,
// mirroring wrftools.tilemap.Z_RASTER/Z_VECTOR - raster imagery paints
// under vector outlines by default so outlines stay legible on top of it.
constexpr int kRasterOverlayZ = 0;
constexpr int kVectorOverlayZ = 100;

// The C++ map owns pan/zoom and tile I/O; raster/vector overlays are
// organized into named, independently replaceable groups (paint ordered by
// each group's z, low-to-under-high) so two tabs sharing one map widget
// (DomainForm's domain outlines, ViewForm's raster layers) can each update
// their own overlays without erasing the other's.
class TileMapWidget final : public QWidget {
public:
    explicit TileMapWidget(QWidget* parent = nullptr);
    void setCenter(double longitude, double latitude, int zoom);
    void zoomToBounds(LonLat southWest, LonLat northEast);
    // The lon/lat box currently visible in the viewport - used by the
    // Domains tab's "Set to Map View Extent" action.
    [[nodiscard]] std::pair<LonLat, LonLat> currentViewBounds() const;
    void setVectorOverlayGroup(const QString& name, std::vector<VectorOverlay> overlays, int z = kVectorOverlayZ);
    void clearVectorOverlayGroup(const QString& name);
    void setRasterOverlayGroup(const QString& name, std::vector<RasterOverlay> overlays, int z = kRasterOverlayZ);
    void clearRasterOverlayGroup(const QString& name);
    void setLegend(QPixmap legend);
    void setInfoText(const QString& text);
    [[nodiscard]] bool exportImage(const QString& path);

    // Test-facing accessors only - mirror the private attributes Python's
    // tests reach into directly on TileMapWidget (there is no real privacy
    // there); production code never needs these.
    [[nodiscard]] std::size_t vectorOverlayGroupSize(const QString& name) const { return static_cast<std::size_t>(vectorGroups_.value(name).overlays.size()); }
    [[nodiscard]] std::size_t rasterOverlayGroupSize(const QString& name) const { return static_cast<std::size_t>(rasterGroups_.value(name).overlays.size()); }
    [[nodiscard]] bool hasVectorOverlayGroup(const QString& name) const { return vectorGroups_.contains(name); }
    [[nodiscard]] bool hasRasterOverlayGroup(const QString& name) const { return rasterGroups_.contains(name); }
    [[nodiscard]] bool hasLegend() const noexcept { return !legend_.isNull(); }
    [[nodiscard]] const QPixmap& legendPixmap() const noexcept { return legend_; }
    [[nodiscard]] bool hasInfoText() const noexcept { return !infoText_.isEmpty(); }
    [[nodiscard]] QRectF legendRect() const noexcept { return legendRect_; }
    [[nodiscard]] QRectF infoRect() const noexcept { return infoRect_; }
    [[nodiscard]] QString dragTarget() const noexcept { return dragTarget_; }
    [[nodiscard]] std::optional<QPointF> legendPosition() const noexcept { return legendPosition_; }
    [[nodiscard]] std::optional<QPointF> infoPosition() const noexcept { return infoPosition_; }
    [[nodiscard]] double centerLongitude() const noexcept { return longitude_; }
    [[nodiscard]] double centerLatitude() const noexcept { return latitude_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QPointF worldPixel() const;
    [[nodiscard]] static QPointF worldPixel(double longitude, double latitude, int zoom);
    [[nodiscard]] static QPointF mercatorWorldPixel(double x, double y, int zoom);
    [[nodiscard]] static QPointF lonLat(QPointF world, int zoom);
    [[nodiscard]] QString tileKey(int x, int y, int zoom) const;
    void ensureTile(int x, int y, int zoom);
    [[nodiscard]] QRectF movableRect(QSizeF size, const std::optional<QPointF>& position, bool topRight) const;

    QNetworkAccessManager network_;
    QHash<QString, QPixmap> tiles_;
    QSet<QString> pending_;
    QString cacheDirectory_;
    struct VectorGroup { int z{}; std::vector<VectorOverlay> overlays; };
    struct RasterGroup { int z{}; std::vector<RasterOverlay> overlays; };
    QHash<QString, VectorGroup> vectorGroups_;
    QHash<QString, RasterGroup> rasterGroups_;
    QPixmap legend_;
    std::optional<QPointF> legendPosition_;
    QRectF legendRect_;
    QString infoText_;
    std::optional<QPointF> infoPosition_;
    QRectF infoRect_;
    QString dragTarget_;  // "" | "legend" | "info"
    QPointF dragOffset_;
    double longitude_{0.0};
    double latitude_{20.0};
    int zoom_{2};
    bool dragging_{false};
    QPointF dragStart_;
};

}  // namespace wrftools
