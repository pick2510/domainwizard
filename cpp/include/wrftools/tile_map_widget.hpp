#pragma once

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
struct LonLat { double longitude{}; double latitude{}; };
struct VectorOverlay { std::vector<LonLat> points; QColor color{Qt::red}; double width{2.0}; };
struct RasterOverlay { QImage image; LonLat southWest; LonLat northEast; double opacity{1.0}; bool smooth{true}; };

// The C++ map owns pan/zoom and tile I/O; raster/vector overlays are added in
// later port phases through the same widget rather than a web-map dependency.
class TileMapWidget final : public QWidget {
public:
    explicit TileMapWidget(QWidget* parent = nullptr);
    void setCenter(double longitude, double latitude, int zoom);
    void setVectorOverlays(std::vector<VectorOverlay> overlays);
    void setRasterOverlays(std::vector<RasterOverlay> overlays);
    void setLegend(QPixmap legend);
    [[nodiscard]] bool exportImage(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QPointF worldPixel() const;
    [[nodiscard]] static QPointF worldPixel(double longitude, double latitude, int zoom);
    [[nodiscard]] static QPointF lonLat(QPointF world, int zoom);
    [[nodiscard]] QString tileKey(int x, int y, int zoom) const;
    void ensureTile(int x, int y, int zoom);
    QNetworkAccessManager network_;
    QHash<QString, QPixmap> tiles_;
    QSet<QString> pending_;
    QString cacheDirectory_;
    std::vector<VectorOverlay> vectorOverlays_;
    std::vector<RasterOverlay> rasterOverlays_;
    QPixmap legend_;
    std::optional<QPointF> legendPosition_;
    QRectF legendRect_;
    bool draggingLegend_{false};
    QPointF legendOffset_;
    double longitude_{0.0};
    double latitude_{20.0};
    int zoom_{2};
    bool dragging_{false};
    QPointF dragStart_;
};

}  // namespace wrftools
