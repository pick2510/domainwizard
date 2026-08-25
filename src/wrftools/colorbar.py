"""Builds a small QPixmap colorbar legend for the View tab's currently
selected layer - either a vertical gradient (build_legend_pixmap, for a
continuous colormap) or a swatch-per-class list (build_categorical_legend_pixmap,
for colormaps.CATEGORICAL) - meant to be handed to TileMapWidget.set_legend()
and drawn fixed in a map corner.

Kept separate from tilemap.py (which knows nothing about colormaps/units)
and from colormaps.py (which knows nothing about Qt) - this is the one
place that bridges the two for this one small piece of UI.
"""
from typing import Dict, Sequence

from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QColor, QFont, QFontMetrics, QImage, QPainter, QPen, QPixmap

from wrftools import colormaps

BAR_WIDTH = 18
BAR_HEIGHT = 120
MARGIN = 8
TITLE_HEIGHT = 16
MIN_TICK_TEXT_WIDTH = 32

# Categorical legend: capped so a large scheme (the MODIS landuse table has
# 24 classes) can't produce a legend taller than the map itself - only
# classes actually present in the layer's data are listed anyway (see
# LayerRenderer.categorical_legend), so this only bites on a genuinely
# high-cardinality field.
MAX_CATEGORICAL_ROWS = 20
SWATCH_SIZE = 12
ROW_HEIGHT = 16


def _title_font() -> QFont:
    font = QFont()
    font.setPointSize(8)
    font.setBold(True)
    return font


def _label_font() -> QFont:
    font = QFont()
    font.setPointSize(8)
    font.setBold(False)
    return font


def _draw_title(painter: QPainter, title: str, width: int) -> None:
    font = _title_font()
    painter.setFont(font)
    painter.setPen(QColor(20, 20, 20))
    fm = QFontMetrics(font)
    elided = fm.elidedText(title, Qt.TextElideMode.ElideRight, width - MARGIN * 2)
    painter.drawText(MARGIN, MARGIN + fm.ascent(), elided)


def _draw_panel_background(painter: QPainter, width: int, height: int) -> None:
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setPen(QPen(QColor(90, 90, 90), 1))
    painter.setBrush(QColor(255, 255, 255, 215))
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1, height - 1), 6, 6)


def build_legend_pixmap(
    colormap_name: str, vmin: float, vmax: float, title: str,
    tick_count: int = 3, tick_format: str = 'auto', tick_decimals: int = 2,
) -> QPixmap:
    """A vertical gradient bar for a continuous colormap, with `tick_count`
    (>= 2) evenly spaced value labels from vmax (top) to vmin (bottom) -
    matching the default (3, 'auto', 2) reproduces the bar's previous fixed
    min/mid/max-only appearance exactly."""
    lut = colormaps.get(colormap_name)
    tick_count = max(2, tick_count)

    tick_values = [vmax - i / (tick_count - 1) * (vmax - vmin) for i in range(tick_count)]
    tick_labels = [_format_tick(v, tick_format, tick_decimals) for v in tick_values]
    fm_tick = QFontMetrics(_label_font())
    # Width is derived from the actual (possibly wide, e.g. scientific
    # notation) labels rather than a fixed constant - a hardcoded width
    # clips '1.23e+02'-style ticks.
    tick_text_width = max(MIN_TICK_TEXT_WIDTH, max(fm_tick.horizontalAdvance(s) for s in tick_labels))

    width = MARGIN * 2 + BAR_WIDTH + 4 + tick_text_width
    height = MARGIN * 2 + TITLE_HEIGHT + BAR_HEIGHT
    pixmap = QPixmap(width, height)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    _draw_panel_background(painter, width, height)
    _draw_title(painter, title, width)

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

    painter.setFont(_label_font())
    painter.setPen(QColor(20, 20, 20))
    tick_x = MARGIN + BAR_WIDTH + 4
    for i, label in enumerate(tick_labels):
        y_center = bar_top + i / (tick_count - 1) * BAR_HEIGHT
        painter.drawText(int(tick_x), int(y_center + fm_tick.ascent() / 2 - 1), label)

    painter.end()
    return pixmap


def build_categorical_legend_pixmap(
    lut, labels: Dict[int, str], present: Sequence[int], title: str,
) -> QPixmap:
    """A color-swatch + class-name row per category value in `present`
    (capped at MAX_CATEGORICAL_ROWS, with a '+N more' row for the rest) -
    the categorical-colormap counterpart of build_legend_pixmap's gradient."""
    shown = list(present[:MAX_CATEGORICAL_ROWS])
    extra = len(present) - len(shown)
    row_labels = [labels.get(v, f'Category {v}') for v in shown]
    if extra > 0:
        row_labels.append(f'+{extra} more')

    fm_label = QFontMetrics(_label_font())
    label_width = max((fm_label.horizontalAdvance(s) for s in row_labels), default=0)

    width = MARGIN * 2 + SWATCH_SIZE + 6 + label_width
    height = MARGIN * 2 + TITLE_HEIGHT + len(row_labels) * ROW_HEIGHT
    pixmap = QPixmap(width, height)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    _draw_panel_background(painter, width, height)
    _draw_title(painter, title, width)

    painter.setFont(_label_font())
    rows_top = MARGIN + TITLE_HEIGHT
    swatch_x = MARGIN
    text_x = MARGIN + SWATCH_SIZE + 6
    for row, value in enumerate(shown):
        row_top = rows_top + row * ROW_HEIGHT
        r, g, b = (int(c) for c in lut[value])
        painter.setPen(QPen(QColor(90, 90, 90), 1))
        painter.setBrush(QColor(r, g, b))
        swatch_y = row_top + (ROW_HEIGHT - SWATCH_SIZE) / 2.0
        painter.drawRect(QRectF(swatch_x, swatch_y, SWATCH_SIZE, SWATCH_SIZE))
        painter.setPen(QColor(20, 20, 20))
        painter.drawText(text_x, int(row_top + fm_label.ascent() + (ROW_HEIGHT - fm_label.height()) / 2.0), row_labels[row])
    if extra > 0:
        row_top = rows_top + len(shown) * ROW_HEIGHT
        painter.setPen(QColor(90, 90, 90))
        painter.drawText(text_x, int(row_top + fm_label.ascent() + (ROW_HEIGHT - fm_label.height()) / 2.0), row_labels[-1])

    painter.end()
    return pixmap


def _format_tick(value: float, fmt: str = 'auto', decimals: int = 2) -> str:
    if fmt == 'fixed':
        return f'{value:.{decimals}f}'
    if fmt == 'scientific':
        return f'{value:.{decimals}e}'
    return f'{value:.3g}'
