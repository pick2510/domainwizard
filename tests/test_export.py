"""tilemap.TileMapWidget.export_image(): saves the current map view (tiles,
overlays, legend, attribution - i.e. whatever paintEvent() draws) to an
image file. See colorbar.py's module docstring for why a QApplication
fixture has to be kept alive, not just constructed inline - QPixmap has the
same requirement.
"""
import os

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import pytest
from PyQt6.QtGui import QImage
from PyQt6.QtWidgets import QApplication

from wrftools.tilemap import TileMapWidget


@pytest.fixture(scope='session')
def qapp():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def map_widget(qapp):
    w = TileMapWidget('https://example.invalid/{z}/{x}/{y}.png')
    w.resize(300, 200)
    w.show()
    return w


def test_export_image_writes_a_readable_nonempty_png(map_widget, tmp_path):
    out_path = str(tmp_path / 'map.png')
    assert map_widget.export_image(out_path) is True

    assert os.path.exists(out_path)
    assert os.path.getsize(out_path) > 0

    image = QImage(out_path)
    assert not image.isNull()
    assert image.width() > 0
    assert image.height() > 0


def test_export_image_to_an_unwritable_path_returns_false(map_widget):
    assert map_widget.export_image('/no/such/directory/map.png') is False
