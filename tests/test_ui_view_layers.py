"""Phase 4 of the View-tab plan: Qt/UI-level coverage of ViewForm through
real widget interaction, matching tests/test_ui_domain_tree.py's rigor and
idioms (QT_QPA_PLATFORM=offscreen, monkeypatched QFileDialog, driving real
widgets rather than calling handlers directly).

Uses the real (tiny) fixtures rather than a fake reader - both are 70-80 KB
and render in well under a millisecond, so there's no need for a fake.

Note: several ViewForm actions (variable/colormap/range changes) rebuild
the layer QTreeWidget from scratch, which invalidates any previously
fetched QTreeWidgetItem - re-fetch via layer_tree.topLevelItem(0) after
such an action rather than holding a stale item reference (the same
lifetime hazard exists in domainform.py and its own tests).

.isVisible() on a child widget requires the whole ancestor chain to have
been shown at least once - not just its own setVisible(True) - so `form`
below calls .show(), unlike DomainForm's test fixture (which doesn't need
visibility assertions).
"""
import os

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import pytest
from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QApplication, QFileDialog

from wrftools.tilemap import TileMapWidget, Z_RASTER
from wrftools.viewform import ViewForm, LAYER_ID_ROLE

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
GEO_EM = os.path.join(FIXTURES_DIR, 'geo_em_small.nc')
WRFOUT = os.path.join(FIXTURES_DIR, 'wrfout_multitime.nc')
SERIES_PATHS = [
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_00_00_00.nc'),
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_00_30_00.nc'),
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_01_00_00.nc'),
]


@pytest.fixture(scope='session')
def qapp():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def map_widget(qapp):
    return TileMapWidget('https://example.invalid/{z}/{x}/{y}.png')


@pytest.fixture
def form(qapp, map_widget):
    f = ViewForm(map_widget)
    f.show()
    return f


def _open(form, path):
    QFileDialog.getOpenFileNames = staticmethod(lambda *a, **k: ([path], ''))
    form.on_open_file_button_clicked()


def _open_multi(form, paths):
    QFileDialog.getOpenFileNames = staticmethod(lambda *a, **k: (paths, ''))
    form.on_open_file_button_clicked()


# --- opening a file and adding a layer ---------------------------------------

def test_open_file_and_add_layer(form, map_widget):
    _open(form, WRFOUT)
    assert form.file_list.topLevelItemCount() == 1
    assert form._open_file_paths() == [WRFOUT]

    form.on_add_layer_button_clicked()
    assert len(form._layers) == 1
    assert len(map_widget.overlay_group('view-rasters')) == 1


def test_add_layer_button_is_enabled_right_after_opening_the_first_file(form):
    # Regression: add_layer_button's enabled state is only updated by
    # _update_panel_visibility(), which _rebuild_layer_tree() calls but
    # _rebuild_file_list() didn't - so opening a file (with no layers yet
    # to trigger a tree rebuild) left the button permanently disabled.
    assert not form.add_layer_button.isEnabled()
    _open(form, WRFOUT)
    assert form.add_layer_button.isEnabled()


def test_adding_a_layer_with_no_open_file_is_a_noop(form):
    form.on_add_layer_button_clicked()
    assert form._layers == []


# --- multi-file series ---------------------------------------------------

def test_multi_selecting_a_series_opens_one_files_entry(form):
    _open_multi(form, SERIES_PATHS)
    assert form.file_list.topLevelItemCount() == 1
    assert form._open_file_paths() == [SERIES_PATHS[0]]
    item = form.file_list.topLevelItem(0)
    assert item.text(0).startswith('wrfout_d01 (3 files')


def test_series_time_combo_shows_real_timestamps(form):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    labels = [form.time_combo.itemText(i) for i in range(form.time_combo.count())]
    assert labels == ['2020-01-01 00:00', '2020-01-01 00:30', '2020-01-01 01:00']


def test_stepping_time_on_a_series_layer_updates_the_map(form, map_widget):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    form.time_combo.setCurrentIndex(2)
    assert form._layers[0].time_index == 2
    overlay = form._renderer.overlay_for(form._layers[0])
    assert overlay is not None


def test_multi_selecting_a_single_file_behaves_like_the_normal_open(form):
    _open_multi(form, [WRFOUT])
    assert form.file_list.topLevelItemCount() == 1
    assert form._open_file_paths() == [WRFOUT]


# --- layer selection and property panel ----------------------------------

def test_selecting_a_layer_shows_its_properties(form):
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()
    item = form.layer_tree.topLevelItem(0)
    form.layer_tree.setCurrentItem(item)
    assert form._selected_layer_id == form._layers[0].layer_id
    assert form.gbox_layer_props.isVisible()
    assert form.variable_combo.count() == len(form._renderer.open_file(WRFOUT).variables)


def test_level_row_hidden_for_2d_variable_shown_for_3d(form):
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()  # default variable is whatever's first
    item = form.layer_tree.topLevelItem(0)
    form.layer_tree.setCurrentItem(item)

    two_d_index = form.variable_combo.findData('T2')
    form.variable_combo.setCurrentIndex(two_d_index)
    assert not form.widget_level.isVisible()

    three_d_index = form.variable_combo.findData('U')
    form.variable_combo.setCurrentIndex(three_d_index)
    assert form.widget_level.isVisible()
    assert form.level_label.text() == 'Vertical Level:'
    assert form.level_combo.count() == 3


# --- time stepping -----------------------------------------------------------

def test_time_combo_selection_updates_the_layer(form):
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    form.time_combo.setCurrentIndex(2)
    assert form._layers[0].time_index == 2


def test_prev_next_time_buttons_step_and_clamp(form):
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    form.next_time_button.click()
    form.next_time_button.click()
    assert form._layers[0].time_index == 2
    form.next_time_button.click()  # already at the last step
    assert form._layers[0].time_index == 2
    form.prev_time_button.click()
    assert form._layers[0].time_index == 1


# --- visibility checkbox ------------------------------------------------------

def test_unchecking_visibility_removes_the_overlay_without_removing_the_layer(form, map_widget):
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()
    assert len(map_widget.overlay_group('view-rasters')) == 1

    item = form.layer_tree.topLevelItem(0)
    item.setCheckState(0, Qt.CheckState.Unchecked)
    assert len(form._layers) == 1
    assert form._layers[0].visible is False
    assert map_widget.overlay_group('view-rasters') == []

    item = form.layer_tree.topLevelItem(0)  # re-fetch: unaffected here, but good hygiene
    item.setCheckState(0, Qt.CheckState.Checked)
    assert len(map_widget.overlay_group('view-rasters')) == 1


# --- colormap / opacity / range ----------------------------------------------

def test_colormap_and_opacity_changes_apply_to_the_layer(form):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    plasma_index = form.colormap_combo.findData('plasma')
    form.colormap_combo.setCurrentIndex(plasma_index)
    assert form._layers[0].colormap == 'plasma'

    form.opacity_slider.setValue(25)
    assert form._layers[0].opacity == pytest.approx(0.25)


def test_interpolate_checkbox_toggles_the_layer_and_overlay_smoothing(form):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    assert form.interpolate_check.isChecked()  # default is True
    assert form._layers[0].interpolate is True

    form.interpolate_check.setChecked(False)
    assert form._layers[0].interpolate is False
    overlay = form._renderer.overlay_for(form._layers[0])
    assert overlay.smooth is False

    form.interpolate_check.setChecked(True)
    assert form._layers[0].interpolate is True
    overlay = form._renderer.overlay_for(form._layers[0])
    assert overlay.smooth is True


# --- categorical colormap auto-detect -----------------------------------

def test_categorical_colormap_auto_selected_for_lu_index(form):
    from wrftools import colormaps

    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()  # default variable is HGT_M (continuous)
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    assert form._layers[0].colormap != colormaps.CATEGORICAL

    lu_index = form.variable_combo.findData('LU_INDEX')
    form.variable_combo.setCurrentIndex(lu_index)
    assert form._layers[0].colormap == colormaps.CATEGORICAL
    assert form.colormap_combo.findData(colormaps.CATEGORICAL) >= 0
    assert form.map_widget._legend is not None  # a discrete swatch legend built successfully


def test_switching_away_from_categorical_variable_reverts_the_colormap(form):
    from wrftools import colormaps

    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    lu_index = form.variable_combo.findData('LU_INDEX')
    form.variable_combo.setCurrentIndex(lu_index)
    assert form._layers[0].colormap == colormaps.CATEGORICAL

    hgt_index = form.variable_combo.findData('HGT_M')
    form.variable_combo.setCurrentIndex(hgt_index)
    assert form._layers[0].colormap != colormaps.CATEGORICAL
    assert form.colormap_combo.findData(colormaps.CATEGORICAL) == -1  # no longer offered


def test_categorical_colormap_can_still_be_manually_overridden(form):
    from wrftools import colormaps

    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    lu_index = form.variable_combo.findData('LU_INDEX')
    form.variable_combo.setCurrentIndex(lu_index)
    assert form._layers[0].colormap == colormaps.CATEGORICAL

    plasma_index = form.colormap_combo.findData('plasma')
    form.colormap_combo.setCurrentIndex(plasma_index)
    assert form._layers[0].colormap == 'plasma'


# --- unit conversion -----------------------------------------------------

def test_units_combo_hidden_for_a_variable_with_no_known_conversions(form):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()  # HGT_M: units is blank in this fixture
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    assert not form.widget_units.isVisible()


def test_units_combo_shown_and_changes_the_layer_and_colorbar_title(form, map_widget):
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    t2_index = form.variable_combo.findData('T2')
    form.variable_combo.setCurrentIndex(t2_index)
    assert form.widget_units.isVisible()  # T2 is Kelvin, convertible

    kelvin_legend = map_widget._legend
    degc_index = form.units_combo.findData('degC')
    assert degc_index >= 0
    form.units_combo.setCurrentIndex(degc_index)
    assert form._layers[0].units == 'degC'
    assert map_widget._legend.toImage() != kelvin_legend.toImage()


# --- colorbar tick/format controls ----------------------------------------

def test_tick_count_spinbox_updates_the_layer_and_the_legend(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    assert form.tick_count_spin.value() == 3  # default
    before = map_widget._legend
    form.tick_count_spin.setValue(7)
    assert form._layers[0].tick_count == 7
    assert map_widget._legend.toImage() != before.toImage()


def test_tick_format_combo_enables_decimals_spinbox_only_when_not_auto(form):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    assert not form.tick_decimals_spin.isEnabled()  # 'auto' by default
    fixed_index = form.tick_format_combo.findData('fixed')
    form.tick_format_combo.setCurrentIndex(fixed_index)
    assert form._layers[0].tick_format == 'fixed'
    assert form.tick_decimals_spin.isEnabled()


# --- colorbar legend ----------------------------------------------------

def test_colorbar_shown_for_selected_visible_layer(form, map_widget):
    assert map_widget._legend is None  # nothing selected yet

    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    assert map_widget._legend is not None


def test_colorbar_hidden_when_selected_layer_is_not_visible(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    assert map_widget._legend is not None

    item = form.layer_tree.topLevelItem(0)
    item.setCheckState(0, Qt.CheckState.Unchecked)
    assert map_widget._legend is None


def test_colorbar_hidden_when_nothing_is_selected(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    assert map_widget._legend is not None

    form.layer_tree.clearSelection()
    assert map_widget._legend is None


def test_colorbar_follows_layer_selection(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()  # layer 1: HGT_M
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()  # layer 2: T2, now selected
    legend_for_t2 = map_widget._legend
    assert legend_for_t2 is not None

    # Re-select layer 1 (bottom row, since the tree shows topmost layer first).
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(1))
    legend_for_hgt = map_widget._legend
    assert legend_for_hgt is not None
    assert legend_for_hgt.toImage() != legend_for_t2.toImage()


def test_manual_range_overrides_auto(form):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    assert form._layers[0].vmin is None  # auto by default
    form.auto_range_check.setChecked(False)
    form.vmin.set_value(10)
    form.vmax.set_value(200)
    form.on_range_changed()
    assert form._layers[0].vmin == 10
    assert form._layers[0].vmax == 200

    form.auto_range_check.setChecked(True)
    assert form._layers[0].vmin is None
    assert form._layers[0].vmax is None


def test_invalid_range_raises_usererror():
    from gis4wrf.core import UserError
    app = QApplication.instance() or QApplication([])
    from wrftools.tilemap import TileMapWidget
    map_widget = TileMapWidget('https://example.invalid/{z}/{x}/{y}.png')
    form = ViewForm(map_widget)
    form.show()
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    form.auto_range_check.setChecked(False)
    form.vmin.set_value(100)
    form.vmax.set_value(0)  # max < min
    with pytest.raises(UserError):
        form.on_range_changed()


# --- layer add/remove/reorder -------------------------------------------------

def test_remove_layer(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    form.on_remove_layer_button_clicked()
    assert form._layers == []
    assert map_widget.overlay_group('view-rasters') == []


def test_move_up_and_down_reorders_layers(form):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()  # layer 1
    form.on_add_layer_button_clicked()  # layer 2 (drawn on top)
    assert [l.layer_id for l in form._layers] == [1, 2]

    # Displayed tree is reversed (topmost row = topmost/last-drawn layer),
    # so row 0 is layer 2.
    top_row_id = form.layer_tree.topLevelItem(0).data(0, LAYER_ID_ROLE)
    assert top_row_id == 2

    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))  # select layer 2
    form.on_move_down_button_clicked()
    assert [l.layer_id for l in form._layers] == [2, 1]

    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(1))  # re-fetch: layer 2, now bottom row
    form.on_move_up_button_clicked()
    assert [l.layer_id for l in form._layers] == [1, 2]


# --- closing a file ------------------------------------------------------

def test_close_file_with_no_layers_needs_no_confirmation(form):
    _open(form, GEO_EM)
    form.file_list.setCurrentItem(form.file_list.topLevelItem(0))
    form.on_close_file_button_clicked()
    assert form._open_file_paths() == []


def test_close_file_removes_its_layers(form, monkeypatch, map_widget):
    from PyQt6.QtWidgets import QMessageBox
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    assert len(form._layers) == 1

    monkeypatch.setattr(QMessageBox, 'question', staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes))
    form.file_list.setCurrentItem(form.file_list.topLevelItem(0))
    form.on_close_file_button_clicked()

    assert form._layers == []
    assert form._open_file_paths() == []
    assert map_widget.overlay_group('view-rasters') == []


# --- coexistence with the rest of the map ------------------------------------

def test_two_layers_from_different_files_both_render(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    _open(form, WRFOUT)
    form.on_add_layer_button_clicked()
    assert len(form._layers) == 2
    assert len(map_widget.overlay_group('view-rasters')) == 2


def test_zoom_to_layer_moves_the_map(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    map_widget.resize(400, 400)
    map_widget.set_center(0.0, 0.0, zoom=2)
    form.on_zoom_to_layer_button_clicked()
    # geo_em_small.nc is centered around lon ~114.17, lat ~22.3 (Hong Kong)
    assert map_widget._center_lon == pytest.approx(114.17, abs=0.1)
    assert map_widget._center_lat == pytest.approx(22.3, abs=0.1)


def test_adding_the_first_layer_auto_zooms_to_it(form, map_widget):
    # Regression: without this, a newly added layer is invisible against the
    # whole-world default view (app.py starts the map at zoom 2) - a WRF
    # domain is typically a tiny sliver of that, often under a pixel wide.
    map_widget.resize(400, 400)
    map_widget.set_center(0.0, 0.0, zoom=2)
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    assert map_widget._center_lon == pytest.approx(114.17, abs=0.1)
    assert map_widget._center_lat == pytest.approx(22.3, abs=0.1)


def test_adding_a_second_layer_does_not_recenter_the_map(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    map_widget.resize(400, 400)
    map_widget.set_center(0.0, 0.0, zoom=2)  # simulate the user having panned away
    form.on_add_layer_button_clicked()  # second layer, same file
    assert map_widget._center_lon == pytest.approx(0.0, abs=0.01)
    assert map_widget._center_lat == pytest.approx(0.0, abs=0.01)


# --- play button ----------------------------------------------------------

def test_play_button_disabled_for_a_single_timestep_file(form):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    assert not form.play_button.isEnabled()


def test_play_button_enabled_for_a_multi_file_series(form):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    assert form.play_button.isEnabled()


def test_checking_play_starts_the_timer_and_advances_time_on_tick(form):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    form.play_button.setChecked(True)
    assert form._play_timer.isActive()
    assert form.time_combo.currentIndex() == 0

    form._advance_play()
    assert form.time_combo.currentIndex() == 1


def test_play_wraps_back_to_the_first_frame_after_the_last(form):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    form.time_combo.setCurrentIndex(form.time_combo.count() - 1)

    form._advance_play()
    assert form.time_combo.currentIndex() == 0


def test_unchecking_play_stops_the_timer(form):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    form.play_button.setChecked(True)
    form.play_button.setChecked(False)
    assert not form._play_timer.isActive()


def test_switching_layer_selection_stops_playback(form):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.on_add_layer_button_clicked()
    items = [form.layer_tree.topLevelItem(i) for i in range(form.layer_tree.topLevelItemCount())]
    form.layer_tree.setCurrentItem(items[0])
    form.play_button.setChecked(True)
    assert form._play_timer.isActive()

    form.layer_tree.setCurrentItem(items[1])
    assert not form.play_button.isChecked()
    assert not form._play_timer.isActive()


# --- movable colorbar / info overlay ---------------------------------------

def test_colorbar_legend_starts_in_the_default_corner_and_can_be_dragged(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))
    map_widget.resize(400, 300)
    map_widget.grab()  # force a paint so _legend_rect reflects the current pixmap

    assert map_widget._legend_pos is None
    default_rect = map_widget._legend_rect
    assert default_rect.right() == pytest.approx(map_widget.width() - 10, abs=1)

    from PyQt6.QtCore import QPointF
    map_widget._legend_pos = QPointF(5, 5)
    map_widget.grab()
    assert map_widget._legend_rect.topLeft() == QPointF(5, 5)


def test_north_arrow_checkbox_toggles_the_map_overlay(form, map_widget):
    _open(form, GEO_EM)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    assert map_widget._north_arrow_visible is False
    form.north_arrow_check.setChecked(True)
    assert map_widget._north_arrow_visible is True
    form.north_arrow_check.setChecked(False)
    assert map_widget._north_arrow_visible is False


def test_info_overlay_hidden_by_default_and_shown_when_checked(form, map_widget):
    _open_multi(form, SERIES_PATHS)
    form.on_add_layer_button_clicked()
    form.layer_tree.setCurrentItem(form.layer_tree.topLevelItem(0))

    assert map_widget._info_text is None

    form.show_info_check.setChecked(True)
    assert map_widget._info_text is not None
    assert '2020-01-01 00:00' in map_widget._info_text

    form.time_combo.setCurrentIndex(1)
    assert '2020-01-01 00:30' in map_widget._info_text

    form.show_info_check.setChecked(False)
    assert map_widget._info_text is None
