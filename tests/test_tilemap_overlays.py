"""Phase 3 of the View-tab plan: tilemap.py's overlay-group mechanism and
raster overlay support. Covers plain-Python coordinate math (no widgets
needed) plus real widget/paint-path checks (offscreen Qt), including the
coexistence guarantee the whole two-tab design depends on: DomainForm's
vector overlays and a raster layer group must be able to update
independently on one shared TileMapWidget.
"""
import math
import os

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import numpy as np
import pytest
from PyQt6.QtCore import QEvent, QPointF, Qt
from PyQt6.QtGui import QColor, QImage, QMouseEvent, QPen, QPixmap
from PyQt6.QtWidgets import QApplication

import gis4wrf.core as core
from wrftools.domainform import DomainForm
from wrftools.tilemap import (
    TileMapWidget, Overlay, RasterOverlay, Z_RASTER, Z_VECTOR,
    lonlat_to_tile_xy, mercator_to_world_px, TILE_SIZE,
)

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
SIBLINGS_WPS = os.path.join(FIXTURES_DIR, 'namelist_siblings.wps')


@pytest.fixture(scope='session')
def qapp():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def map_widget(qapp):
    return TileMapWidget('https://example.invalid/{z}/{x}/{y}.png')


def _lonlat_to_mercator(lon: float, lat: float):
    x = math.radians(lon) * 6378137.0
    y = math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)) * 6378137.0
    return x, y


# --- coordinate agreement (no widgets) ---------------------------------------

@pytest.mark.parametrize('lon,lat', [
    (0.0, 0.0), (114.17, 22.3), (-122.4, 37.8), (6.62, 46.5), (-179.9, 84.9), (179.9, -84.9),
])
@pytest.mark.parametrize('zoom', [0, 2, 8, 15])
def test_mercator_to_world_px_agrees_with_lonlat_to_tile_xy(lon, lat, zoom):
    expected_x, expected_y = lonlat_to_tile_xy(lon, lat, zoom)
    expected = (expected_x * TILE_SIZE, expected_y * TILE_SIZE)

    merc_x, merc_y = _lonlat_to_mercator(lon, lat)
    actual = mercator_to_world_px(merc_x, merc_y, zoom)

    assert actual[0] == pytest.approx(expected[0], abs=1e-6)
    assert actual[1] == pytest.approx(expected[1], abs=1e-6)


# --- overlay groups -----------------------------------------------------------

def test_overlay_groups_are_independent(map_widget):
    vector_overlay = Overlay(rings=[[(0.0, 0.0), (1.0, 0.0), (1.0, 1.0)]], pen=QPen(QColor('red')))
    map_widget.set_overlay_group('domains', [vector_overlay], z=Z_VECTOR)
    assert len(map_widget.overlay_group('domains')) == 1
    assert map_widget.overlay_group('view-rasters') == []

    raster = RasterOverlay(QImage(2, 2, QImage.Format.Format_RGBA8888), (0, 0, 1, 1))
    map_widget.set_overlay_group('view-rasters', [raster], z=Z_RASTER)

    # Setting one group must not disturb the other.
    assert len(map_widget.overlay_group('domains')) == 1
    assert len(map_widget.overlay_group('view-rasters')) == 1

    map_widget.clear_overlay_group('domains')
    assert map_widget.overlay_group('domains') == []
    assert len(map_widget.overlay_group('view-rasters')) == 1


def test_legend_defaults_to_none_and_can_be_set_and_cleared(map_widget):
    assert map_widget._legend is None

    pixmap = QPixmap(10, 10)
    map_widget.set_legend(pixmap)
    assert map_widget._legend is pixmap

    map_widget.set_legend(None)
    assert map_widget._legend is None


def test_info_text_defaults_to_none_and_can_be_set_and_cleared(map_widget):
    assert map_widget._info_text is None

    map_widget.set_info_text('T2 (degC)  —  2020-01-01 00:30')
    assert map_widget._info_text == 'T2 (degC)  —  2020-01-01 00:30'

    map_widget.set_info_text(None)
    assert map_widget._info_text is None


# --- movable legend / info overlay -------------------------------------------

def test_legend_defaults_to_top_right_corner(map_widget):
    map_widget.resize(400, 300)
    map_widget.set_legend(QPixmap(60, 30))
    map_widget.grab()  # forces a real paintEvent, populating _legend_rect
    assert map_widget._legend_pos is None
    assert map_widget._legend_rect.right() == pytest.approx(map_widget.width() - 10, abs=1)
    assert map_widget._legend_rect.top() == pytest.approx(10, abs=1)


def test_info_text_defaults_to_top_left_corner(map_widget):
    map_widget.resize(400, 300)
    map_widget.set_info_text('T2 (degC)')
    map_widget.grab()
    assert map_widget._info_pos is None
    assert map_widget._info_rect.left() == pytest.approx(10, abs=1)
    assert map_widget._info_rect.top() == pytest.approx(10, abs=1)


def test_dragging_the_legend_moves_it_and_does_not_pan_the_map(map_widget):
    map_widget.resize(400, 300)
    map_widget.set_center(10.0, 20.0, zoom=4)
    map_widget.set_legend(QPixmap(60, 30))
    map_widget.grab()

    center = map_widget._legend_rect.center()
    offset = center - map_widget._legend_rect.topLeft()
    _press(map_widget, center)
    assert map_widget._drag_target == 'legend'
    _move(map_widget, QPointF(20, 250))
    map_widget.grab()
    assert map_widget._legend_pos == QPointF(20, 250) - offset
    _release(map_widget, QPointF(20, 250))
    assert map_widget._drag_target is None

    # A drag targeting the legend must not have panned the underlying map.
    assert map_widget._center_lon == pytest.approx(10.0)
    assert map_widget._center_lat == pytest.approx(20.0)


def test_dragging_the_info_overlay_moves_it_independently_of_the_legend(map_widget):
    map_widget.resize(400, 300)
    map_widget.set_legend(QPixmap(60, 30))
    map_widget.set_info_text('T2 (degC)')
    map_widget.grab()

    legend_pos_before = map_widget._legend_pos
    center = map_widget._info_rect.center()
    _press(map_widget, center)
    assert map_widget._drag_target == 'info'
    _move(map_widget, QPointF(150, 150))
    map_widget.grab()
    _release(map_widget, QPointF(150, 150))

    assert map_widget._info_pos is not None
    assert map_widget._legend_pos == legend_pos_before  # untouched


def test_north_arrow_hidden_by_default_shown_at_bottom_left_when_enabled(map_widget):
    map_widget.resize(400, 300)
    assert map_widget._north_arrow_visible is False
    map_widget.grab()
    assert map_widget._north_arrow_rect.isEmpty()

    map_widget.set_north_arrow(True)
    map_widget.grab()
    assert map_widget._north_arrow_pos is None
    assert map_widget._north_arrow_rect.left() == pytest.approx(10, abs=1)
    assert map_widget._north_arrow_rect.bottom() == pytest.approx(map_widget.height() - 10, abs=1)

    map_widget.set_north_arrow(False)
    map_widget.grab()
    assert map_widget._north_arrow_rect.isEmpty()


def test_dragging_the_north_arrow_moves_it_and_does_not_pan_the_map(map_widget):
    map_widget.resize(400, 300)
    map_widget.set_center(10.0, 20.0, zoom=4)
    map_widget.set_north_arrow(True)
    map_widget.grab()

    center = map_widget._north_arrow_rect.center()
    _press(map_widget, center)
    assert map_widget._drag_target == 'north_arrow'
    _move(map_widget, QPointF(200, 100))
    map_widget.grab()
    _release(map_widget, QPointF(200, 100))

    assert map_widget._north_arrow_pos is not None
    assert map_widget._center_lon == pytest.approx(10.0)
    assert map_widget._center_lat == pytest.approx(20.0)


def test_dragging_empty_map_area_still_pans_as_before(map_widget):
    map_widget.resize(400, 300)
    map_widget.set_center(0.0, 0.0, zoom=4)
    map_widget.set_legend(QPixmap(60, 30))
    map_widget.grab()

    _press(map_widget, QPointF(200, 150))  # far from the legend's top-right box
    assert map_widget._drag_target is None
    assert map_widget._dragging is True
    _move(map_widget, QPointF(150, 150))
    _release(map_widget, QPointF(150, 150))

    assert (map_widget._center_lon, map_widget._center_lat) != (0.0, 0.0)


def _press(map_widget, pos):
    event = QMouseEvent(
        QEvent.Type.MouseButtonPress, pos, pos, Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
    map_widget.mousePressEvent(event)


def _move(map_widget, pos):
    event = QMouseEvent(
        QEvent.Type.MouseMove, pos, pos, Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
    map_widget.mouseMoveEvent(event)


def _release(map_widget, pos):
    event = QMouseEvent(
        QEvent.Type.MouseButtonRelease, pos, pos, Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier)
    map_widget.mouseReleaseEvent(event)


def test_raster_group_paints_before_vector_group(map_widget):
    """Rasters must sit under vector outlines - verified via paint order,
    not pixels: the raster group's z must sort before the vector group's."""
    map_widget.set_overlay_group('view-rasters', [], z=Z_RASTER)
    map_widget.set_overlay_group('domains', [], z=Z_VECTOR)
    ordered = sorted(map_widget._groups.items(), key=lambda kv: (kv[1][0], kv[0]))
    order = [name for name, _ in ordered]
    assert order.index('view-rasters') < order.index('domains')


def test_map_widget_paints_with_both_groups_populated(map_widget):
    """A real paint pass (offscreen) with both a raster and a vector overlay
    present must not raise - exercises the full polymorphic paint dispatch."""
    map_widget.resize(64, 64)
    map_widget.set_center(0.0, 0.0, zoom=4)

    buffer = np.zeros((4, 4, 4), dtype=np.uint8)
    buffer[..., 3] = 255
    image = QImage(buffer.data, 4, 4, 4 * 4, QImage.Format.Format_RGBA8888)
    map_widget.set_overlay_group(
        'view-rasters',
        [RasterOverlay(image, (-1000.0, -1000.0, 1000.0, 1000.0), opacity=0.5, _buffer=buffer)],
        z=Z_RASTER)
    map_widget.set_overlay_group(
        'domains',
        [Overlay(rings=[[(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0)]], pen=QPen(QColor('red')))],
        z=Z_VECTOR)

    map_widget.grab()  # forces a real paintEvent offscreen


def test_raster_overlay_outside_viewport_is_skipped_without_error(map_widget):
    map_widget.resize(64, 64)
    map_widget.set_center(0.0, 0.0, zoom=4)
    far_away = RasterOverlay(QImage(2, 2, QImage.Format.Format_RGBA8888), (10_000_000.0, 10_000_000.0, 10_000_001.0, 10_000_001.0))
    map_widget.set_overlay_group('view-rasters', [far_away], z=Z_RASTER)
    map_widget.grab()


# --- coexistence with a real DomainForm --------------------------------------

def test_domain_outlines_and_raster_layers_coexist_on_shared_map(qapp, map_widget):
    """The design this whole plan depends on: DomainForm and a raster-layer
    consumer (standing in for ViewForm, not yet built) sharing one
    TileMapWidget must not be able to erase each other's overlays."""
    form = DomainForm(map_widget)
    nml = core.read_namelist(SIBLINGS_WPS, 'wps')
    form.project = core.convert_wps_nml_to_project(nml, form.project)

    assert len(map_widget.overlay_group('domains')) > 0
    assert map_widget.overlay_group('view-rasters') == []

    buffer = np.zeros((2, 2, 4), dtype=np.uint8)
    buffer[..., 3] = 255
    image = QImage(buffer.data, 2, 2, 2 * 4, QImage.Format.Format_RGBA8888)
    map_widget.set_overlay_group('view-rasters', [RasterOverlay(image, (0, 0, 1, 1), _buffer=buffer)], z=Z_RASTER)

    # Adding the raster layer must not have touched the domain outlines...
    assert len(map_widget.overlay_group('domains')) > 0
    # ...and editing a domain field (root is selected after the import
    # above) must not touch the raster layer.
    assert form._selected_domain_number == 1
    root = form._project.data['domains'][0]
    form.center_lon.set_value(root['center_lonlat'][0] + 0.1)
    form._apply_selected_domain_fields(raise_on_invalid=False)
    assert len(map_widget.overlay_group('view-rasters')) == 1
