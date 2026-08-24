"""colorbar.py: builds the View tab's on-map colorbar legend as a QPixmap.

QPixmap (unlike QImage) needs a live QApplication to exist - and it must
stay alive for as long as any QPixmap built under it is in use. A bare
`QApplication.instance() or QApplication([])` expression whose result isn't
kept anywhere is not enough: with no reference held, the QApplication is
garbage-collected (destroying the underlying object) immediately after the
statement returns, and the very next QPixmap construction aborts the
process with "QPixmap: Must construct a QGuiApplication before a QPixmap" -
hit this for real while writing this module, hence the fixture below rather
than a plain helper function.
"""
import os

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import pytest
from PyQt6.QtWidgets import QApplication

from domainwizard import colorbar


@pytest.fixture(scope='session')
def qapp():
    return QApplication.instance() or QApplication([])


def test_builds_a_nonempty_pixmap(qapp):
    pixmap = colorbar.build_legend_pixmap('viridis', vmin=0.0, vmax=10.0, title='T2 (K)')
    assert not pixmap.isNull()
    assert pixmap.width() > 0
    assert pixmap.height() > 0


def test_different_colormaps_produce_different_pixels(qapp):
    viridis = colorbar.build_legend_pixmap('viridis', vmin=0.0, vmax=10.0, title='T2 (K)')
    plasma = colorbar.build_legend_pixmap('plasma', vmin=0.0, vmax=10.0, title='T2 (K)')
    assert viridis.toImage() != plasma.toImage()


def test_different_ranges_produce_different_pixels(qapp):
    narrow = colorbar.build_legend_pixmap('viridis', vmin=0.0, vmax=10.0, title='T2 (K)')
    wide = colorbar.build_legend_pixmap('viridis', vmin=0.0, vmax=100.0, title='T2 (K)')
    assert narrow.toImage() != wide.toImage()


def test_format_tick_uses_significant_digits():
    assert colorbar._format_tick(291.123456) == '291'
    assert colorbar._format_tick(0.000123456) == '0.000123'
