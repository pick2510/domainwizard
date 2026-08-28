#pragma once

#include "wrftools/crs.hpp"

#include <cstddef>
#include <functional>
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

class QComboBox;
class QPainterPath;

namespace wrftools {
// A selectable basemap tile source - name/url/attribution/maxZoom mirror the
// fields of a QGIS "connections-xyz" entry ({x}/{y}/{z} placeholders in url,
// plus {q} for a Bing-style quadkey in place of x/y/z). `tms` is carried
// through from that entry's format for completeness but not currently
// applied - every built-in provider here uses the standard top-origin XYZ
// scheme (Bing instead uses {q}, handled separately), so there has been no
// provider yet that actually needs the TMS y-flip.
struct TileProvider {
    QString name;
    QString url;
    QString attribution;
    int maxZoom{19};
    bool tms{false};
};
// The built-in basemap choices, in display order. First entry
// ("OpenStreetMap Standard") is the app's long-standing default.
[[nodiscard]] std::vector<TileProvider> builtinTileProviders();

// LonLat is defined in crs.hpp (lon/lat fields) and reused here.
// handles is empty for a plain outline; a caller that wants its polygon
// resizable via on-map corner handles (DomainForm's domain outlines) fills
// in exactly 4 corner points (order: SW, SE, NE, NW - see
// setOverlayResizeHandlers) computed from its own authoritative bounds,
// rather than this widget trying to infer "corners" from an arbitrary,
// densified ring.
// fill is transparent (alpha 0) by default, matching every existing caller
// (domain/AOI outlines were stroke-only until the Reproject tab's shaded
// domain footprint needed a filled polygon too) - only closed overlays with
// a non-transparent fill are actually filled (see paintEvent).
struct VectorOverlay { std::vector<LonLat> points; QColor color{Qt::red}; double width{2.0}; bool closed{false}; std::vector<LonLat> handles{}; QColor fill{Qt::transparent}; };
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
    [[nodiscard]] const std::vector<TileProvider>& tileProviders() const noexcept { return providers_; }
    [[nodiscard]] int currentTileProviderIndex() const noexcept { return currentProviderIndex_; }
    [[nodiscard]] const TileProvider& currentTileProvider() const { return providers_.at(static_cast<std::size_t>(currentProviderIndex_)); }
    // Switches the basemap - clears no state but re-fetches/re-renders under
    // the new provider's own tile cache namespace, so switching back is
    // instant if the old tiles are still cached.
    void setTileProvider(int index);
    [[nodiscard]] QComboBox* tileProviderCombo() const noexcept { return providerCombo_; }
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

    // Optional per-pixel readout drawn near the bottom-left corner while the
    // mouse is over the map. Given the lon/lat under the cursor, the
    // handler returns a formatted "value" string to prefix onto the
    // coordinates (e.g. "12.3 degC"), or nullopt to fall back to showing
    // just the coordinates - this widget knows nothing about rasters or
    // units, it only asks. Called on every mouse-move, so the handler
    // should be cheap (ViewForm's implementation samples an already-warped,
    // cached slice - no file I/O). Deliberately excluded from
    // exportImage()'s output, unlike the legend/info overlays.
    using HoverValueHandler = std::function<std::optional<QString>(LonLat)>;
    void setHoverValueHandler(HoverValueHandler handler);

    // Static "N" compass arrow in the bottom-right corner - this widget
    // never rotates the basemap, so north is always straight up. Off by
    // default; included in exportImage()'s output, like the legend.
    void setShowNorthArrow(bool show);
    [[nodiscard]] bool showNorthArrow() const noexcept { return showNorthArrow_; }

    [[nodiscard]] bool exportImage(const QString& path);

    // Marks one vector overlay group as drag-enabled (DomainForm uses this
    // for "domains") - a press landing inside one of that group's closed
    // polygons starts a drag instead of panning the map. This widget knows
    // nothing about what a "domain" is: it just reports which polygon (by
    // index within the group, topmost/last-in-the-group first so a nested
    // polygon wins over the parent it sits inside) was grabbed and the
    // lon/lat under the cursor: onStart once at press, onMove on every
    // subsequent move, onEnd at release. Pass an empty group name to
    // disable (e.g. while a tab that doesn't own this group is active).
    using OverlayDragStartHandler = std::function<void(std::size_t overlayIndex, LonLat pressLonLat)>;
    using OverlayDragMoveHandler = std::function<void(std::size_t overlayIndex, LonLat currentLonLat)>;
    using OverlayDragEndHandler = std::function<void()>;
    void setDraggableVectorOverlayGroup(const QString& groupName);
    void setOverlayDragHandlers(OverlayDragStartHandler onStart, OverlayDragMoveHandler onMove, OverlayDragEndHandler onEnd);

    // Same idea, for the small square handles at an overlay's own
    // VectorOverlay::handles points (drawn for every overlay in the
    // draggable group, not just the one currently being dragged/resized -
    // an always-visible affordance, like the legend's own resize handle).
    // A press landing on one of them takes priority over a body drag/pan.
    // handleIndex is that point's index within the overlay's handles list.
    using OverlayResizeStartHandler = std::function<void(std::size_t overlayIndex, std::size_t handleIndex, LonLat pressLonLat)>;
    using OverlayResizeMoveHandler = std::function<void(std::size_t overlayIndex, std::size_t handleIndex, LonLat currentLonLat)>;
    using OverlayResizeEndHandler = std::function<void()>;
    void setOverlayResizeHandlers(OverlayResizeStartHandler onStart, OverlayResizeMoveHandler onMove, OverlayResizeEndHandler onEnd);

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
    [[nodiscard]] const QString& infoText() const noexcept { return infoText_; }
    [[nodiscard]] QRectF legendRect() const noexcept { return legendRect_; }
    [[nodiscard]] QRectF legendResizeHandleRect() const noexcept { return legendResizeHandleRect_; }
    [[nodiscard]] double legendScale() const noexcept { return legendScale_; }
    [[nodiscard]] QRectF infoRect() const noexcept { return infoRect_; }
    [[nodiscard]] QString dragTarget() const noexcept { return dragTarget_; }
    [[nodiscard]] std::optional<QPointF> legendPosition() const noexcept { return legendPosition_; }
    [[nodiscard]] std::optional<QPointF> infoPosition() const noexcept { return infoPosition_; }
    [[nodiscard]] const QString& draggableVectorOverlayGroup() const noexcept { return draggableGroup_; }
    [[nodiscard]] double centerLongitude() const noexcept { return longitude_; }
    [[nodiscard]] double centerLatitude() const noexcept { return latitude_; }
    [[nodiscard]] const QString& hoverText() const noexcept { return hoverText_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    [[nodiscard]] QPointF worldPixel() const;
    [[nodiscard]] static QPointF worldPixel(double longitude, double latitude, int zoom);
    [[nodiscard]] static QPointF mercatorWorldPixel(double x, double y, int zoom);
    [[nodiscard]] static QPointF lonLat(QPointF world, int zoom);
    [[nodiscard]] QPointF viewportTopLeft() const;
    [[nodiscard]] QString tileKey(int x, int y, int zoom) const;
    void ensureTile(int x, int y, int zoom);
    [[nodiscard]] QRectF movableRect(QSizeF size, const std::optional<QPointF>& position, bool topRight) const;
    [[nodiscard]] static QString quadKey(int x, int y, int zoom);
    void repositionOverlayControls();
    // Ray-casting hit test in screen space against groupName's overlays,
    // last-in-the-group first (see setDraggableVectorOverlayGroup). Sets
    // outIndex and returns true on the first (topmost) containing polygon.
    [[nodiscard]] bool hitTestOverlay(const QString& groupName, QPointF screenPoint, std::size_t& outIndex) const;
    // Same idea for the small square handles at each overlay's
    // VectorOverlay::handles points - screen-distance test (not
    // point-in-polygon), topmost overlay first, first-matching-handle
    // within it.
    [[nodiscard]] bool hitTestOverlayHandle(const QString& groupName, QPointF screenPoint, std::size_t& outOverlayIndex, std::size_t& outHandleIndex) const;
    // Screen-space QPainterPath for one overlay's lon/lat ring, given the
    // viewport's world-pixel top-left - shared by paintEvent's normal
    // vector-overlay pass and its drag-highlight pass so they always trace
    // the exact same outline.
    [[nodiscard]] QPainterPath overlayPath(const VectorOverlay& overlay, QPointF topLeft) const;

    std::vector<TileProvider> providers_;
    int currentProviderIndex_{0};
    QComboBox* providerCombo_{};
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
    // Uniform scale applied to legend_'s own pixel size when drawing/hit-
    // testing - dragging the resize handle in legendResizeHandleRect_
    // changes this rather than re-rendering the pixmap itself, so an
    // existing colorbar can be enlarged/shrunk without waiting on
    // ViewForm's next updateColorbar() call.
    double legendScale_{1.0};
    QRectF legendResizeHandleRect_;
    QPointF legendResizeOrigin_;  // legendRect_.topLeft() captured at press time
    QString infoText_;
    std::optional<QPointF> infoPosition_;
    QRectF infoRect_;
    QString dragTarget_;  // "" | "legend" | "legend-resize" | "info" | "overlay" | "overlay-resize"
    QPointF dragOffset_;
    QString draggableGroup_;
    OverlayDragStartHandler overlayDragStart_;
    OverlayDragMoveHandler overlayDragMove_;
    OverlayDragEndHandler overlayDragEnd_;
    std::size_t draggedOverlayIndex_{};
    OverlayResizeStartHandler overlayResizeStart_;
    OverlayResizeMoveHandler overlayResizeMove_;
    OverlayResizeEndHandler overlayResizeEnd_;
    std::size_t resizedOverlayIndex_{};
    std::size_t resizedHandleIndex_{};
    double longitude_{0.0};
    double latitude_{20.0};
    int zoom_{2};
    bool dragging_{false};
    QPointF dragStart_;
    HoverValueHandler hoverValueHandler_;
    QString hoverText_;
    bool showNorthArrow_{false};
};

}  // namespace wrftools
