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

from wrftools import colorbar, colormaps


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


def test_default_tick_args_reproduce_the_original_three_tick_output(qapp):
    explicit = colorbar.build_legend_pixmap('viridis', 0.0, 10.0, 'T2 (K)', tick_count=3, tick_format='auto', tick_decimals=2)
    default = colorbar.build_legend_pixmap('viridis', 0.0, 10.0, 'T2 (K)')
    assert explicit.toImage() == default.toImage()


def test_tick_count_changes_the_pixmap(qapp):
    three = colorbar.build_legend_pixmap('viridis', 0.0, 10.0, 'T2 (K)', tick_count=3)
    seven = colorbar.build_legend_pixmap('viridis', 0.0, 10.0, 'T2 (K)', tick_count=7)
    assert three.toImage() != seven.toImage()


def test_format_tick_fixed_uses_the_requested_decimals():
    assert colorbar._format_tick(3.14159, 'fixed', 2) == '3.14'
    assert colorbar._format_tick(3.14159, 'fixed', 0) == '3'


def test_format_tick_scientific_uses_the_requested_decimals():
    assert colorbar._format_tick(12345.0, 'scientific', 2) == '1.23e+04'


def test_scientific_ticks_do_not_get_clipped_by_a_hardcoded_width(qapp):
    # A hardcoded 56px tick column (the old constant) is too narrow for
    # something like '1.23e+04' - the bar width must grow to fit it.
    narrow = colorbar.build_legend_pixmap('viridis', 0.0, 99999.0, 'V', tick_format='auto')
    wide = colorbar.build_legend_pixmap('viridis', 0.0, 99999.0, 'V', tick_format='scientific', tick_decimals=4)
    assert wide.width() > narrow.width()


def test_categorical_legend_pixmap_is_nonempty(qapp):
    lut, labels = colormaps.categorical_lut('MODIFIED_IGBP_MODIS_NOAH', 1, 20)
    pixmap = colorbar.build_categorical_legend_pixmap(lut, labels, [1, 2, 17], 'LU_INDEX')
    assert not pixmap.isNull()
    assert pixmap.width() > 0
    assert pixmap.height() > 0


def test_categorical_legend_caps_rows_with_a_more_indicator(qapp):
    lut, labels = colormaps.categorical_lut('MODIFIED_IGBP_MODIS_NOAH', 1, 24)
    present = list(range(1, 25))  # more than MAX_CATEGORICAL_ROWS
    capped = colorbar.build_categorical_legend_pixmap(lut, labels, present, 'LU_INDEX')
    full = colorbar.build_categorical_legend_pixmap(lut, labels, present[:colorbar.MAX_CATEGORICAL_ROWS], 'LU_INDEX')
    # +1 row for the "+N more" line beyond the capped rows.
    assert capped.height() == full.height() + colorbar.ROW_HEIGHT
