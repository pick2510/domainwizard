"""A minimal slippy-map (XYZ tile) widget with no third-party map library.

Draws standard Web Mercator tiles (the same scheme used by OpenStreetMap,
Stamen, etc.) with mouse pan/zoom, plus a simple overlay mechanism for
drawing extra geometries (e.g. WRF/WPS domain outlines) on top in lon/lat
coordinates. Tiles are fetched over HTTP via Qt's own QtNetwork module and
cached to disk, so no extra HTTP library is needed.
"""

import math
import os
from typing import Dict, List, Optional, Sequence, Tuple

from PyQt6.QtCore import QPointF, QRectF, QStandardPaths, Qt
from PyQt6.QtGui import QBrush, QColor, QPainter, QPainterPath, QPen, QPixmap, QWheelEvent, QMouseEvent, QPaintEvent, QResizeEvent
from PyQt6.QtNetwork import QNetworkAccessManager, QNetworkReply, QNetworkRequest
from PyQt6.QtWidgets import QWidget
from PyQt6.QtCore import QUrl

TILE_SIZE = 256
MIN_ZOOM = 0
MAX_ZOOM = 19

LonLat = Tuple[float, float]


def lonlat_to_tile_xy(lon: float, lat: float, zoom: int) -> Tuple[float, float]:
    """Fractional tile coordinates for a lon/lat pair at a given zoom level."""
    lat = max(min(lat, 85.05112878), -85.05112878)
    lat_rad = math.radians(lat)
    n = 2.0 ** zoom
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n
    return x, y


def tile_xy_to_lonlat(x: float, y: float, zoom: int) -> LonLat:
    """Inverse of lonlat_to_tile_xy."""
    n = 2.0 ** zoom
    lon = x / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * y / n))))
    return lon, lat


class Overlay:
    """A set of lon/lat polygons or polylines to draw on top of the tiles."""

    def __init__(self, rings: Sequence[Sequence[LonLat]], pen: QPen, brush: Optional[QBrush] = None, closed: bool = True) -> None:
        self.rings = rings
        self.pen = pen
        self.brush = brush
        self.closed = closed


class TileMapWidget(QWidget):
    """A pannable/zoomable XYZ tile map with lon/lat overlay support."""

    def __init__(
        self,
        tile_url_template: str,
        attribution: str = '',
        cache_dir: Optional[str] = None,
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self.setMouseTracking(True)
        self.setMinimumSize(200, 200)

        self._tile_url_template = tile_url_template
        self._attribution = attribution

        self._center_lon = 0.0
        self._center_lat = 20.0
        self._zoom = 2

        self._overlays: List[Overlay] = []

        self._pixmap_cache: Dict[Tuple[int, int, int], QPixmap] = {}
        self._pending: Dict[Tuple[int, int, int], QNetworkReply] = {}
        self._missing: set = set()  # tiles the server returned an error for; don't retry every paint

        if cache_dir is None:
            base = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.CacheLocation)
            cache_dir = os.path.join(base or '.', 'domainwizard', 'tiles')
        self._cache_dir = cache_dir
        os.makedirs(self._cache_dir, exist_ok=True)

        self._network = QNetworkAccessManager(self)

        self._dragging = False
        self._drag_last_pos = QPointF()

    # --- public API -----------------------------------------------------

    def set_center(self, lon: float, lat: float, zoom: Optional[int] = None) -> None:
        self._center_lon = lon
        self._center_lat = lat
        if zoom is not None:
            self._zoom = max(MIN_ZOOM, min(MAX_ZOOM, zoom))
        self.update()

    def set_overlays(self, overlays: List[Overlay]) -> None:
        self._overlays = overlays
        self.update()

    def fit_bounds(self, min_lon: float, min_lat: float, max_lon: float, max_lat: float, padding_frac: float = 0.1) -> None:
        """Center the view on a lon/lat bounding box and pick a zoom level that fits it."""
        self._center_lon = (min_lon + max_lon) / 2.0
        self._center_lat = (min_lat + max_lat) / 2.0

        width = max(self.width(), 1)
        height = max(self.height(), 1)

        for zoom in range(MAX_ZOOM, MIN_ZOOM - 1, -1):
            x0, y0 = lonlat_to_tile_xy(min_lon, max_lat, zoom)
            x1, y1 = lonlat_to_tile_xy(max_lon, min_lat, zoom)
            px_width = abs(x1 - x0) * TILE_SIZE
            px_height = abs(y1 - y0) * TILE_SIZE
            if px_width <= width * (1 - padding_frac) and px_height <= height * (1 - padding_frac):
                self._zoom = zoom
                break
        else:
            self._zoom = MIN_ZOOM
        self.update()

    def current_view_bbox(self) -> Tuple[float, float, float, float]:
        """Returns (min_lon, min_lat, max_lon, max_lat) of the currently visible area."""
        top_left = self.screen_to_lonlat(QPointF(0, 0))
        bottom_right = self.screen_to_lonlat(QPointF(self.width(), self.height()))
        min_lon, max_lon = sorted([top_left[0], bottom_right[0]])
        min_lat, max_lat = sorted([top_left[1], bottom_right[1]])
        return min_lon, min_lat, max_lon, max_lat

    def screen_to_lonlat(self, pos: QPointF) -> LonLat:
        cx, cy = lonlat_to_tile_xy(self._center_lon, self._center_lat, self._zoom)
        center_world_px = QPointF(cx * TILE_SIZE, cy * TILE_SIZE)
        top_left_world_px = center_world_px - QPointF(self.width() / 2.0, self.height() / 2.0)
        world_px = top_left_world_px + pos
        return tile_xy_to_lonlat(world_px.x() / TILE_SIZE, world_px.y() / TILE_SIZE, self._zoom)

    def lonlat_to_screen(self, lon: float, lat: float) -> QPointF:
        cx, cy = lonlat_to_tile_xy(self._center_lon, self._center_lat, self._zoom)
        center_world_px = QPointF(cx * TILE_SIZE, cy * TILE_SIZE)
        top_left_world_px = center_world_px - QPointF(self.width() / 2.0, self.height() / 2.0)
        x, y = lonlat_to_tile_xy(lon, lat, self._zoom)
        world_px = QPointF(x * TILE_SIZE, y * TILE_SIZE)
        return world_px - top_left_world_px

    # --- painting ---------------------------------------------------------

    def paintEvent(self, event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(220, 220, 220))

        cx, cy = lonlat_to_tile_xy(self._center_lon, self._center_lat, self._zoom)
        center_world_px = QPointF(cx * TILE_SIZE, cy * TILE_SIZE)
        top_left_world_px = center_world_px - QPointF(self.width() / 2.0, self.height() / 2.0)

        n = 2 ** self._zoom
        first_tile_x = math.floor(top_left_world_px.x() / TILE_SIZE)
        first_tile_y = math.floor(top_left_world_px.y() / TILE_SIZE)
        last_tile_x = math.floor((top_left_world_px.x() + self.width()) / TILE_SIZE)
        last_tile_y = math.floor((top_left_world_px.y() + self.height()) / TILE_SIZE)

        for tile_y in range(first_tile_y, last_tile_y + 1):
            if tile_y < 0 or tile_y >= n:
                continue
            for tile_x in range(first_tile_x, last_tile_x + 1):
                wrapped_x = tile_x % n
                screen_x = tile_x * TILE_SIZE - top_left_world_px.x()
                screen_y = tile_y * TILE_SIZE - top_left_world_px.y()
                pixmap = self._get_tile(self._zoom, wrapped_x, tile_y)
                if pixmap is not None:
                    painter.drawPixmap(int(screen_x), int(screen_y), pixmap)
                else:
                    painter.fillRect(QRectF(screen_x, screen_y, TILE_SIZE, TILE_SIZE), QColor(230, 230, 230))

        self._paint_overlays(painter, top_left_world_px)

        if self._attribution:
            painter.setPen(QColor(0, 0, 0))
            painter.fillRect(QRectF(0, self.height() - 18, self.width(), 18), QColor(255, 255, 255, 180))
            painter.drawText(4, self.height() - 5, self._attribution)

    def _paint_overlays(self, painter: QPainter, top_left_world_px: QPointF) -> None:
        for overlay in self._overlays:
            painter.setPen(overlay.pen)
            painter.setBrush(overlay.brush if overlay.brush is not None else QBrush(Qt.BrushStyle.NoBrush))
            for ring in overlay.rings:
                path = QPainterPath()
                for i, (lon, lat) in enumerate(ring):
                    x, y = lonlat_to_tile_xy(lon, lat, self._zoom)
                    screen_pt = QPointF(x * TILE_SIZE, y * TILE_SIZE) - top_left_world_px
                    if i == 0:
                        path.moveTo(screen_pt)
                    else:
                        path.lineTo(screen_pt)
                if overlay.closed and ring:
                    path.closeSubpath()
                painter.drawPath(path)

    # --- tile fetch/cache ---------------------------------------------------

    def _get_tile(self, zoom: int, x: int, y: int) -> Optional[QPixmap]:
        key = (zoom, x, y)
        cached = self._pixmap_cache.get(key)
        if cached is not None:
            return cached

        disk_path = os.path.join(self._cache_dir, str(zoom), str(x), f'{y}.png')
        if os.path.exists(disk_path):
            pixmap = QPixmap(disk_path)
            if not pixmap.isNull():
                self._pixmap_cache[key] = pixmap
                return pixmap

        if key in self._pending or key in self._missing:
            return None

        url = self._tile_url_template.format(z=zoom, x=x, y=y)
        request = QNetworkRequest(QUrl(url))
        # Tile providers such as OpenStreetMap require a descriptive User-Agent
        # and will otherwise block or rate-limit requests.
        request.setHeader(QNetworkRequest.KnownHeaders.UserAgentHeader, 'domainwizard/0.1 (GIS4WRF domain wizard prototype)')
        reply = self._network.get(request)
        self._pending[key] = reply
        reply.finished.connect(lambda: self._on_tile_downloaded(key, reply, disk_path))
        return None

    def _on_tile_downloaded(self, key: Tuple[int, int, int], reply: QNetworkReply, disk_path: str) -> None:
        self._pending.pop(key, None)
        if reply.error() != QNetworkReply.NetworkError.NoError:
            self._missing.add(key)
            reply.deleteLater()
            return

        data = reply.readAll()
        pixmap = QPixmap()
        if pixmap.loadFromData(bytes(data)):
            self._pixmap_cache[key] = pixmap
            os.makedirs(os.path.dirname(disk_path), exist_ok=True)
            with open(disk_path, 'wb') as f:
                f.write(bytes(data))
            self.update()
        else:
            self._missing.add(key)
        reply.deleteLater()

    # --- mouse interaction ---------------------------------------------------

    def wheelEvent(self, event: QWheelEvent) -> None:
        anchor_lon, anchor_lat = self.screen_to_lonlat(event.position())
        delta = 1 if event.angleDelta().y() > 0 else -1
        new_zoom = max(MIN_ZOOM, min(MAX_ZOOM, self._zoom + delta))
        if new_zoom == self._zoom:
            return
        self._zoom = new_zoom
        # Keep the point under the cursor fixed by recentering.
        ax, ay = lonlat_to_tile_xy(anchor_lon, anchor_lat, self._zoom)
        anchor_world_px = QPointF(ax * TILE_SIZE, ay * TILE_SIZE)
        offset_from_center = event.position() - QPointF(self.width() / 2.0, self.height() / 2.0)
        new_center_world_px = anchor_world_px - offset_from_center
        self._center_lon, self._center_lat = tile_xy_to_lonlat(
            new_center_world_px.x() / TILE_SIZE, new_center_world_px.y() / TILE_SIZE, self._zoom)
        self.update()

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = True
            self._drag_last_pos = event.position()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if not self._dragging:
            return
        delta = event.position() - self._drag_last_pos
        self._drag_last_pos = event.position()

        cx, cy = lonlat_to_tile_xy(self._center_lon, self._center_lat, self._zoom)
        center_world_px = QPointF(cx * TILE_SIZE, cy * TILE_SIZE)
        new_center_world_px = center_world_px - delta
        self._center_lon, self._center_lat = tile_xy_to_lonlat(
            new_center_world_px.x() / TILE_SIZE, new_center_world_px.y() / TILE_SIZE, self._zoom)
        self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = False

    def resizeEvent(self, event: QResizeEvent) -> None:
        super().resizeEvent(event)
        self.update()
