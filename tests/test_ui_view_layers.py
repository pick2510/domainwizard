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

from domainwizard.tilemap import TileMapWidget, Z_RASTER
from domainwizard.viewform import ViewForm, LAYER_ID_ROLE

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
GEO_EM = os.path.join(FIXTURES_DIR, 'geo_em_small.nc')
WRFOUT = os.path.join(FIXTURES_DIR, 'wrfout_multitime.nc')


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
    QFileDialog.getOpenFileName = staticmethod(lambda *a, **k: (path, ''))
    form.on_open_file_button_clicked()


# --- opening a file and adding a layer ---------------------------------------

def test_open_file_and_add_layer(form, map_widget):
    _open(form, WRFOUT)
    assert form.file_list.topLevelItemCount() == 1
    assert form._open_file_paths() == [WRFOUT]

    form.on_add_layer_button_clicked()
    assert len(form._layers) == 1
    assert len(map_widget.overlay_group('view-rasters')) == 1


def test_adding_a_layer_with_no_open_file_is_a_noop(form):
    form.on_add_layer_button_clicked()
    assert form._layers == []


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
    from domainwizard.tilemap import TileMapWidget
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
