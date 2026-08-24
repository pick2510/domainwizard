"""Builds a small QPixmap colorbar legend for the View tab's currently
selected layer - a vertical gradient (built from the same colormap LUT used
to render the layer itself) with its value range and variable label, meant
to be handed to TileMapWidget.set_legend() and drawn fixed in a map corner.

Kept separate from tilemap.py (which knows nothing about colormaps/units)
and from colormaps.py (which knows nothing about Qt) - this is the one
place that bridges the two for this one small piece of UI.
"""
from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QColor, QFontMetrics, QImage, QPainter, QPen, QPixmap

from domainwizard import colormaps

BAR_WIDTH = 18
BAR_HEIGHT = 120
MARGIN = 8
TITLE_HEIGHT = 16
TICK_TEXT_WIDTH = 56


def build_legend_pixmap(colormap_name: str, vmin: float, vmax: float, title: str) -> QPixmap:
    lut = colormaps.get(colormap_name)

    width = MARGIN * 2 + BAR_WIDTH + 4 + TICK_TEXT_WIDTH
    height = MARGIN * 2 + TITLE_HEIGHT + BAR_HEIGHT
    pixmap = QPixmap(width, height)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    painter.setPen(QPen(QColor(90, 90, 90), 1))
    painter.setBrush(QColor(255, 255, 255, 215))
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1, height - 1), 6, 6)

    title_font = painter.font()
    title_font.setPointSize(8)
    title_font.setBold(True)
    painter.setFont(title_font)
    painter.setPen(QColor(20, 20, 20))
    fm_title = QFontMetrics(title_font)
    elided = fm_title.elidedText(title, Qt.TextElideMode.ElideRight, width - MARGIN * 2)
    painter.drawText(MARGIN, MARGIN + fm_title.ascent(), elided)

    bar_top = MARGIN + TITLE_HEIGHT
    # Row 0 (top of the drawn bar) is the highest value, so the LUT is
    # sampled top-down from its last entry (vmax) to its first (vmin).
    gradient = QImage(1, 256, QImage.Format.Format_RGB888)
    for row in range(256):
        r, g, b = (int(c) for c in lut[255 - row])
        gradient.setPixelColor(0, row, QColor(r, g, b))
    painter.drawImage(QRectF(MARGIN, bar_top, BAR_WIDTH, BAR_HEIGHT), gradient)
    painter.setPen(QPen(QColor(90, 90, 90), 1))
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawRect(QRectF(MARGIN, bar_top, BAR_WIDTH, BAR_HEIGHT))

    tick_font = painter.font()
    tick_font.setBold(False)
    tick_font.setPointSize(8)
    painter.setFont(tick_font)
    painter.setPen(QColor(20, 20, 20))
    fm_tick = QFontMetrics(tick_font)
    tick_x = MARGIN + BAR_WIDTH + 4

    def draw_tick(value: float, y_center: float) -> None:
        painter.drawText(int(tick_x), int(y_center + fm_tick.ascent() / 2 - 1), _format_tick(value))

    draw_tick(vmax, bar_top)
    draw_tick((vmin + vmax) / 2.0, bar_top + BAR_HEIGHT / 2.0)
    draw_tick(vmin, bar_top + BAR_HEIGHT)

    painter.end()
    return pixmap


def _format_tick(value: float) -> str:
    return f'{value:.3g}'
