"""View tab: opens WRF/WPS NetCDF files (geo_em*, met_em*, wrfinput*,
wrfout*) and draws a configurable stack of colored raster layers on the
shared map, one per (file, variable, time, level, colormap, opacity)
selection - see the View-tab plan.

Follows domainform.py's conventions throughout: stacked QGroupBox sections
in one QVBoxLayout, @pyqtSlot() handlers named on_<thing>_<event>, a
_update_panel_visibility()/_populate_properties_panel() pair with
blockSignals around writes, and UserError for user-facing failures (caught
by errorhandling.py's excepthook, same as DomainForm). Unlike DomainForm
(one domain tree, one selection concept), this tab manages a list of
independent layers plus a set of open files, so its state is a bit more
involved - but the widget-building and redraw-funnel shape is the same.
"""
import math
from typing import List, Optional

from PyQt6.QtCore import Qt, QTimer, pyqtSlot
from PyQt6.QtGui import QDoubleValidator
from PyQt6.QtWidgets import (
    QCheckBox, QComboBox, QFileDialog, QGridLayout, QGroupBox, QHBoxLayout,
    QLabel, QMessageBox, QPushButton, QSlider, QSpinBox, QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget,
)

from gis4wrf.core import UserError

from wrftools.tilemap import TileMapWidget, Z_RASTER
from wrftools.rasterlayer import LayerRenderer, RasterLayer
from wrftools import colorbar, colormaps
from wrftools import units as units_module
from wrftools.formhelpers import add_grid_lineedit
from wrftools.wrfreader import WRFFile, WRFVariable
from wrftools import wrfseries

DECIMALS = 50
RANGE_VALIDATOR = QDoubleValidator(-1e30, 1e30, DECIMALS)

# Time between frames while "Play" is running (see on_play_button_toggled) -
# fast enough to feel like an animation, slow enough that each frame's
# raster (colormap + warp, cached but not free on a cache miss) has time to
# actually finish before the next tick fires.
PLAY_INTERVAL_MS = 600

# EPSG:3857's spherical-Mercator radius (matches tilemap.py's MERC_HALF /
# pi), used only to convert a layer's bounds back to lon/lat for "Zoom to
# Layer" - the map widget's own fit_bounds() takes lon/lat, not metres.
_MERC_RADIUS = 6378137.0

# Qt.ItemDataRole.UserRole on each layer's QTreeWidgetItem holds its
# RasterLayer.layer_id, mirroring domainform.py's DOMAIN_NUMBER_ROLE.
LAYER_ID_ROLE = Qt.ItemDataRole.UserRole


def _mercator_to_lonlat(x: float, y: float) -> tuple:
    lon = math.degrees(x / _MERC_RADIUS)
    lat = math.degrees(2 * math.atan(math.exp(y / _MERC_RADIUS)) - math.pi / 2)
    return lon, lat


class ViewForm(QWidget):
    def __init__(self, map_widget: TileMapWidget, renderer: Optional[LayerRenderer] = None) -> None:
        super().__init__()
        self.map_widget = map_widget
        self._renderer = renderer if renderer is not None else LayerRenderer()
        self._active = True

        self._layers: List[RasterLayer] = []  # bottom-first: draw order
        self._next_layer_id = 1
        self._selected_layer_id: Optional[int] = None

        # --- Files -------------------------------------------------------
        self.file_list = QTreeWidget()
        self.file_list.setHeaderHidden(True)
        self.file_list.itemSelectionChanged.connect(self.on_file_selection_changed)
        open_file_button = QPushButton('Open NetCDF File...')
        open_file_button.clicked.connect(self.on_open_file_button_clicked)
        self.close_file_button = QPushButton('Close File')
        self.close_file_button.clicked.connect(self.on_close_file_button_clicked)
        self.close_file_button.setEnabled(False)
        vbox_files = QVBoxLayout()
        vbox_files.addWidget(self.file_list)
        vbox_files.addWidget(open_file_button)
        vbox_files.addWidget(self.close_file_button)
        self.gbox_files = QGroupBox('Files')
        self.gbox_files.setLayout(vbox_files)

        # --- Layers --------------------------------------------------------
        self.layer_tree = QTreeWidget()
        self.layer_tree.setHeaderHidden(True)
        self.layer_tree.itemSelectionChanged.connect(self.on_layer_selection_changed)
        self.layer_tree.itemChanged.connect(self.on_layer_item_changed)
        self.add_layer_button = QPushButton('Add Layer')
        self.add_layer_button.clicked.connect(self.on_add_layer_button_clicked)
        self.remove_layer_button = QPushButton('Remove Layer')
        self.remove_layer_button.clicked.connect(self.on_remove_layer_button_clicked)
        self.move_up_button = QPushButton('Move Up')
        self.move_up_button.clicked.connect(self.on_move_up_button_clicked)
        self.move_down_button = QPushButton('Move Down')
        self.move_down_button.clicked.connect(self.on_move_down_button_clicked)
        move_buttons = QHBoxLayout()
        move_buttons.addWidget(self.move_up_button)
        move_buttons.addWidget(self.move_down_button)
        vbox_layers = QVBoxLayout()
        vbox_layers.addWidget(self.layer_tree)
        vbox_layers.addWidget(self.add_layer_button)
        vbox_layers.addWidget(self.remove_layer_button)
        vbox_layers.addLayout(move_buttons)
        self.gbox_layers = QGroupBox('Layers')
        self.gbox_layers.setLayout(vbox_layers)

        # --- Layer properties ------------------------------------------------
        self.gbox_layer_props = QGroupBox('Layer Properties')
        grid_props = QGridLayout()

        self.variable_combo = QComboBox()
        self.variable_combo.currentIndexChanged.connect(self.on_variable_changed)
        grid_props.addWidget(QLabel('Variable:'), 0, 0)
        grid_props.addWidget(self.variable_combo, 0, 1, 1, 2)

        self.time_combo = QComboBox()
        self.time_combo.currentIndexChanged.connect(self.on_time_changed)
        self.prev_time_button = QPushButton('‹')
        self.prev_time_button.clicked.connect(self.on_prev_time_button_clicked)
        self.next_time_button = QPushButton('›')
        self.next_time_button.clicked.connect(self.on_next_time_button_clicked)
        # Only meaningful (and only enabled - see _populate_time_combo) for
        # a layer with more than one timestep, i.e. mainly the multi-file
        # WRF output series case (wrfseries.WRFFileSeries) this was added
        # for, though any multi-timestep file qualifies too.
        self.play_button = QPushButton('▶')
        self.play_button.setCheckable(True)
        self.play_button.setEnabled(False)
        self.play_button.toggled.connect(self.on_play_button_toggled)
        grid_props.addWidget(QLabel('Time:'), 1, 0)
        grid_props.addWidget(self.time_combo, 1, 1)
        hbox_time_buttons = QHBoxLayout()
        hbox_time_buttons.addWidget(self.prev_time_button)
        hbox_time_buttons.addWidget(self.next_time_button)
        hbox_time_buttons.addWidget(self.play_button)
        grid_props.addLayout(hbox_time_buttons, 1, 2)

        self._play_timer = QTimer(self)
        self._play_timer.setInterval(PLAY_INTERVAL_MS)
        self._play_timer.timeout.connect(self._advance_play)

        self.level_label = QLabel('Level:')
        self.level_combo = QComboBox()
        self.level_combo.currentIndexChanged.connect(self.on_level_changed)
        level_row = QHBoxLayout()
        level_row.addWidget(self.level_label)
        level_row.addWidget(self.level_combo)
        self.widget_level = QWidget()
        self.widget_level.setLayout(level_row)
        grid_props.addWidget(self.widget_level, 2, 0, 1, 3)

        # Hidden (like widget_level above) whenever the selected variable's
        # native unit has no known conversions - units.conversions_for()
        # returns just the identity entry in that case, so there's nothing
        # to pick between.
        self.units_label = QLabel('Units:')
        self.units_combo = QComboBox()
        self.units_combo.currentIndexChanged.connect(self.on_units_changed)
        units_row = QHBoxLayout()
        units_row.addWidget(self.units_label)
        units_row.addWidget(self.units_combo)
        self.widget_units = QWidget()
        self.widget_units.setLayout(units_row)
        grid_props.addWidget(self.widget_units, 3, 0, 1, 3)

        # Rebuilt per-selection (see _populate_colormap_combo), not populated
        # once here: the "Categorical" entry only appears for a variable
        # with a category_scheme (RasterLayer/wrfreader.WRFVariable).
        self.colormap_combo = QComboBox()
        self.colormap_combo.currentIndexChanged.connect(self.on_colormap_changed)
        grid_props.addWidget(QLabel('Colormap:'), 4, 0)
        grid_props.addWidget(self.colormap_combo, 4, 1, 1, 2)

        self.opacity_slider = QSlider(Qt.Orientation.Horizontal)
        self.opacity_slider.setRange(0, 100)
        self.opacity_slider.valueChanged.connect(self.on_opacity_changed)
        self.opacity_label = QLabel()
        grid_props.addWidget(QLabel('Opacity:'), 5, 0)
        grid_props.addWidget(self.opacity_slider, 5, 1)
        grid_props.addWidget(self.opacity_label, 5, 2)

        self.auto_range_check = QCheckBox('Auto')
        self.auto_range_check.toggled.connect(self.on_range_changed)
        grid_props.addWidget(QLabel('Range:'), 6, 0)
        grid_props.addWidget(self.auto_range_check, 6, 1)

        grid_range = QGridLayout()
        self.vmin = add_grid_lineedit(grid_range, 0, 'Min', RANGE_VALIDATOR)
        self.vmax = add_grid_lineedit(grid_range, 1, 'Max', RANGE_VALIDATOR)
        for field in (self.vmin, self.vmax):
            field.editingFinished.connect(self.on_range_changed)
        grid_props.addLayout(grid_range, 7, 0, 1, 3)

        self.interpolate_check = QCheckBox('Interpolate')
        self.interpolate_check.toggled.connect(self.on_interpolate_changed)
        grid_props.addWidget(self.interpolate_check, 8, 0, 1, 3)

        self.gbox_layer_props.setLayout(grid_props)

        # --- Colorbar ------------------------------------------------------
        # Legend appearance only (see RasterLayer's tick_count/tick_format/
        # tick_decimals docstring) - a separate group box from Layer
        # Properties above since it configures how the range is *displayed*,
        # not the layer's data/rendering itself.
        self.gbox_colorbar = QGroupBox('Colorbar')
        grid_colorbar = QGridLayout()

        self.tick_count_spin = QSpinBox()
        self.tick_count_spin.setRange(2, 11)
        self.tick_count_spin.valueChanged.connect(self.on_tick_count_changed)
        grid_colorbar.addWidget(QLabel('Ticks:'), 0, 0)
        grid_colorbar.addWidget(self.tick_count_spin, 0, 1)

        self.tick_format_combo = QComboBox()
        self.tick_format_combo.addItem('Auto', 'auto')
        self.tick_format_combo.addItem('Fixed', 'fixed')
        self.tick_format_combo.addItem('Scientific', 'scientific')
        self.tick_format_combo.currentIndexChanged.connect(self.on_tick_format_changed)
        grid_colorbar.addWidget(QLabel('Format:'), 1, 0)
        grid_colorbar.addWidget(self.tick_format_combo, 1, 1)

        self.tick_decimals_spin = QSpinBox()
        self.tick_decimals_spin.setRange(0, 10)
        self.tick_decimals_spin.valueChanged.connect(self.on_tick_decimals_changed)
        grid_colorbar.addWidget(QLabel('Decimals:'), 2, 0)
        grid_colorbar.addWidget(self.tick_decimals_spin, 2, 1)

        # A separate, independently movable overlay (see TileMapWidget.
        # set_info_text) from the colorbar itself - shows the selected
        # layer's variable/unit/time as one draggable label, e.g.
        # "T2 (degC) - 2025-03-14 00:30", handy once the colorbar has been
        # dragged away from its default corner and no longer doubles as an
        # at-a-glance readout of what's on screen.
        self.show_info_check = QCheckBox('Show Info Overlay')
        self.show_info_check.toggled.connect(self.on_show_info_toggled)
        grid_colorbar.addWidget(self.show_info_check, 3, 0, 1, 2)

        self.gbox_colorbar.setLayout(grid_colorbar)

        # --- View --------------------------------------------------------
        self.gbox_zoom = QGroupBox('View')
        vbox_zoom = QVBoxLayout()
        zoom_to_layer_button = QPushButton('Zoom to Layer')
        zoom_to_layer_button.clicked.connect(self.on_zoom_to_layer_button_clicked)
        vbox_zoom.addWidget(zoom_to_layer_button)
        self.gbox_zoom.setLayout(vbox_zoom)

        layout = QVBoxLayout()
        layout.addWidget(self.gbox_files)
        layout.addWidget(self.gbox_layers)
        layout.addWidget(self.gbox_layer_props)
        layout.addWidget(self.gbox_colorbar)
        layout.addWidget(self.gbox_zoom)
        layout.addStretch(1)
        self.setLayout(layout)

        self._update_panel_visibility()

    def set_active(self, active: bool) -> None:
        """Camera-move gate for the shared map widget - see tilemap's
        module docstring for why two tabs need this. Overlay redraws
        (refresh_map) are unaffected: only fit_bounds() calls triggered by
        non-user-initiated events (none currently exist here, but this
        keeps ViewForm consistent with DomainForm's mechanism) are gated."""
        self._active = active

    # --- files -----------------------------------------------------------

    @pyqtSlot()
    def on_open_file_button_clicked(self) -> None:
        # Multi-select so a whole WRF output series (one file per timestep,
        # e.g. wrfout_d03_2025-03-14_00_00_00, ..._00_30_00, ...) can be
        # opened in one go - group_paths() groups same-domain files sharing
        # a recognized WRF/WPS naming pattern into a single series entry
        # (wrfseries.WRFFileSeries) automatically; anything else (or a
        # single selected file, the common case) opens exactly as before.
        file_paths, _ = QFileDialog.getOpenFileNames(self, caption='Open WRF/WPS NetCDF file(s)')
        if not file_paths:
            return
        groups, singles = wrfseries.group_paths(file_paths)
        select_path = None
        for group in groups:
            select_path = self._renderer.open_files(group).path  # raises UserError on a mismatched series
        for path in singles:
            self._renderer.open_file(path)  # raises UserError on a bad file
            select_path = path
        self._rebuild_file_list(select_path=select_path)

    @pyqtSlot()
    def on_close_file_button_clicked(self) -> None:
        selected = self.file_list.selectedItems()
        if not selected:
            return
        file_path = selected[0].data(0, LAYER_ID_ROLE)
        affected = [layer for layer in self._layers if layer.file_path == file_path]
        if affected:
            answer = QMessageBox.question(
                self, 'Close File',
                f'{len(affected)} layer(s) use this file. Closing it will remove them. Continue?',
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return

        self._layers = [layer for layer in self._layers if layer.file_path != file_path]
        self._renderer.invalidate_file(file_path)
        self._rebuild_file_list()
        self._rebuild_layer_tree()
        self.refresh_map()

    def _rebuild_file_list(self, select_path: Optional[str] = None) -> None:
        self.file_list.blockSignals(True)
        self.file_list.clear()
        for path in self._open_file_paths():
            # Goes through the renderer's own name (not a raw path split) so
            # a multi-file series (wrfseries.WRFFileSeries) shows its
            # descriptive name ("wrfout_d03 (12 files, ...)") instead of
            # looking like just its first file.
            item = QTreeWidgetItem([self._renderer.open_file(path).name])
            item.setData(0, LAYER_ID_ROLE, path)
            self.file_list.addTopLevelItem(item)
            if path == select_path:
                item.setSelected(True)
        self.file_list.blockSignals(False)
        self.on_file_selection_changed()
        # add_layer_button's enabled state depends on whether any file is
        # open, not on layer selection - without this, opening the very
        # first file leaves it permanently disabled (nothing else would
        # ever call _update_panel_visibility() to flip it on).
        self._update_panel_visibility()

    def _open_file_paths(self) -> List[str]:
        return self._renderer.open_paths

    @pyqtSlot()
    def on_file_selection_changed(self) -> None:
        self.close_file_button.setEnabled(bool(self.file_list.selectedItems()))

    # --- layers ------------------------------------------------------------

    @pyqtSlot()
    def on_add_layer_button_clicked(self) -> None:
        paths = self._open_file_paths()
        if not paths:
            return
        selected = self.file_list.selectedItems()
        file_path = selected[0].data(0, LAYER_ID_ROLE) if selected else paths[0]
        wrf_file = self._renderer.open_file(file_path)
        if not wrf_file.variables:
            return
        variable = next(iter(wrf_file.variables))

        is_first_layer = not self._layers
        layer = RasterLayer(layer_id=self._next_layer_id, file_path=file_path, variable=variable)
        if wrf_file.variables[variable].category_scheme is not None:
            layer.colormap = colormaps.CATEGORICAL
        self._next_layer_id += 1
        self._layers.append(layer)
        self._rebuild_layer_tree(select_id=layer.layer_id)
        self.refresh_map()
        if is_first_layer:
            # A WRF domain is typically a tiny sliver of the whole-world
            # default view (app.py starts the map at zoom 2), often under a
            # pixel wide - so without this, the very first layer someone
            # adds is invisible and looks like nothing happened.
            self._zoom_to_layer(layer)

    @pyqtSlot()
    def on_remove_layer_button_clicked(self) -> None:
        if self._selected_layer_id is None:
            return
        self._layers = [layer for layer in self._layers if layer.layer_id != self._selected_layer_id]
        self._rebuild_layer_tree()
        self.refresh_map()

    @pyqtSlot()
    def on_move_up_button_clicked(self) -> None:
        self._move_selected_layer(+1)

    @pyqtSlot()
    def on_move_down_button_clicked(self) -> None:
        self._move_selected_layer(-1)

    def _move_selected_layer(self, direction: int) -> None:
        index = self._index_of_selected_layer()
        if index is None:
            return
        new_index = index + direction
        if not (0 <= new_index < len(self._layers)):
            return
        self._layers[index], self._layers[new_index] = self._layers[new_index], self._layers[index]
        self._rebuild_layer_tree(select_id=self._selected_layer_id)
        self.refresh_map()

    def _index_of_selected_layer(self) -> Optional[int]:
        if self._selected_layer_id is None:
            return None
        for index, layer in enumerate(self._layers):
            if layer.layer_id == self._selected_layer_id:
                return index
        return None

    def _layer_label(self, layer: RasterLayer) -> str:
        """RasterLayer.label()'s format, but through the renderer's own
        .name (like _rebuild_file_list's file labels) rather than a raw path
        split - a layer built from a multi-file series shows the series'
        descriptive name instead of just its first file's basename."""
        file_name = self._renderer.open_file(layer.file_path).name
        return f'{layer.variable} — {file_name} (t={layer.time_index + 1})'

    def _rebuild_layer_tree(self, select_id: Optional[int] = None) -> None:
        self._updating_tree = True
        self.layer_tree.blockSignals(True)
        self.layer_tree.clear()
        # self._layers is bottom-first (draw order); display topmost first,
        # matching the usual GIS convention that the top row is the top layer.
        for layer in reversed(self._layers):
            item = QTreeWidgetItem([self._layer_label(layer)])
            item.setData(0, LAYER_ID_ROLE, layer.layer_id)
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(0, Qt.CheckState.Checked if layer.visible else Qt.CheckState.Unchecked)
            self.layer_tree.addTopLevelItem(item)
            if layer.layer_id == select_id:
                item.setSelected(True)
        self.layer_tree.blockSignals(False)
        self._updating_tree = False

        if select_id is not None and any(layer.layer_id == select_id for layer in self._layers):
            self._selected_layer_id = select_id
        else:
            self._selected_layer_id = None
            self.play_button.setChecked(False)  # the layer it was playing may no longer exist
        self._update_panel_visibility()
        self._populate_properties_panel()

    @pyqtSlot()
    def on_layer_selection_changed(self) -> None:
        selected = self.layer_tree.selectedItems()
        self._selected_layer_id = selected[0].data(0, LAYER_ID_ROLE) if selected else None
        # Playback is tied to whichever layer's time_index is advancing -
        # switching layers mid-play would silently start driving a
        # different layer's time, which is surprising, so just stop it.
        self.play_button.setChecked(False)
        self._update_panel_visibility()
        self._populate_properties_panel()
        # Changing the selection alone (no property edit) doesn't go through
        # refresh_map(), but the colorbar tracks the selected layer, so it
        # needs its own update here.
        self._update_colorbar()

    @pyqtSlot('QTreeWidgetItem*', int)
    def on_layer_item_changed(self, item: QTreeWidgetItem, column: int) -> None:
        if getattr(self, '_updating_tree', False):
            return
        layer_id = item.data(0, LAYER_ID_ROLE)
        layer = self._selected_layer(layer_id)
        if layer is None:
            return
        layer.visible = item.checkState(0) == Qt.CheckState.Checked
        self.refresh_map()

    def _selected_layer(self, layer_id: Optional[int] = None) -> Optional[RasterLayer]:
        target = layer_id if layer_id is not None else self._selected_layer_id
        if target is None:
            return None
        for layer in self._layers:
            if layer.layer_id == target:
                return layer
        return None

    def _update_panel_visibility(self) -> None:
        has_selection = self._selected_layer_id is not None
        self.gbox_layer_props.setVisible(has_selection)
        self.gbox_colorbar.setVisible(has_selection)
        self.gbox_zoom.setVisible(has_selection)

        has_layers = bool(self._layers)
        self.remove_layer_button.setEnabled(has_selection)
        index = self._index_of_selected_layer()
        self.move_up_button.setEnabled(has_selection and index is not None and index < len(self._layers) - 1)
        self.move_down_button.setEnabled(has_selection and index is not None and index > 0)
        self.add_layer_button.setEnabled(bool(self._open_file_paths()))

    # --- layer properties ----------------------------------------------------

    def _populate_properties_panel(self) -> None:
        layer = self._selected_layer()
        if layer is None:
            return
        wrf_file = self._renderer.open_file(layer.file_path)

        self.variable_combo.blockSignals(True)
        self.variable_combo.clear()
        for name, var in wrf_file.variables.items():
            label = name
            if var.units:
                label += f' ({var.units})'
            self.variable_combo.addItem(label, name)
        index = self.variable_combo.findData(layer.variable)
        self.variable_combo.setCurrentIndex(max(0, index))
        self.variable_combo.blockSignals(False)

        self._populate_time_combo(wrf_file, layer)
        self._populate_level_combo(wrf_file, layer)
        self._populate_units_combo(wrf_file, layer)
        self._populate_colormap_combo(wrf_file, layer)

        self.opacity_slider.blockSignals(True)
        self.opacity_slider.setValue(round(layer.opacity * 100))
        self.opacity_label.setText(f'{round(layer.opacity * 100)}%')
        self.opacity_slider.blockSignals(False)

        self.auto_range_check.blockSignals(True)
        self.auto_range_check.setChecked(layer.vmin is None and layer.vmax is None)
        self.auto_range_check.blockSignals(False)
        self.vmin.setEnabled(not self.auto_range_check.isChecked())
        self.vmax.setEnabled(not self.auto_range_check.isChecked())
        if layer.vmin is not None:
            self.vmin.set_value(layer.vmin)
        if layer.vmax is not None:
            self.vmax.set_value(layer.vmax)

        self.interpolate_check.blockSignals(True)
        self.interpolate_check.setChecked(layer.interpolate)
        self.interpolate_check.blockSignals(False)

        self.tick_count_spin.blockSignals(True)
        self.tick_count_spin.setValue(layer.tick_count)
        self.tick_count_spin.blockSignals(False)

        self.tick_format_combo.blockSignals(True)
        self.tick_format_combo.setCurrentIndex(max(0, self.tick_format_combo.findData(layer.tick_format)))
        self.tick_format_combo.blockSignals(False)

        self.tick_decimals_spin.blockSignals(True)
        self.tick_decimals_spin.setValue(layer.tick_decimals)
        self.tick_decimals_spin.setEnabled(layer.tick_format != 'auto')
        self.tick_decimals_spin.blockSignals(False)

    def _populate_units_combo(self, wrf_file: WRFFile, layer: RasterLayer) -> None:
        var = wrf_file.variables[layer.variable]
        options = units_module.conversions_for(var.units)
        is_convertible = len(options) > 1
        self.widget_units.setVisible(is_convertible)
        if not is_convertible:
            return
        self.units_combo.blockSignals(True)
        self.units_combo.clear()
        for unit in options:
            self.units_combo.addItem(unit.label, unit.key)
        target_key = layer.units if layer.units is not None else 'native'
        self.units_combo.setCurrentIndex(max(0, self.units_combo.findData(target_key)))
        self.units_combo.blockSignals(False)

    def _populate_colormap_combo(self, wrf_file: WRFFile, layer: RasterLayer) -> None:
        """Rebuilt per-selection rather than populated once: the
        'Categorical' entry is only offered for a variable with a
        category_scheme (see wrfreader.WRFVariable), so it appears and
        disappears as the selected layer's variable changes."""
        var = wrf_file.variables[layer.variable]
        self.colormap_combo.blockSignals(True)
        self.colormap_combo.clear()
        for name in colormaps.names():
            self.colormap_combo.addItem(name, name)
        if var.category_scheme is not None:
            self.colormap_combo.addItem('Categorical', colormaps.CATEGORICAL)
        self.colormap_combo.setCurrentIndex(max(0, self.colormap_combo.findData(layer.colormap)))
        self.colormap_combo.blockSignals(False)

    def _populate_time_combo(self, wrf_file: WRFFile, layer: RasterLayer) -> None:
        self.time_combo.blockSignals(True)
        self.time_combo.clear()
        for label in wrf_file.times:
            self.time_combo.addItem(label)
        self.time_combo.setCurrentIndex(min(layer.time_index, self.time_combo.count() - 1))
        self.time_combo.blockSignals(False)
        self.play_button.setEnabled(self.time_combo.count() > 1)

    def _populate_level_combo(self, wrf_file: WRFFile, layer: RasterLayer) -> None:
        var = wrf_file.variables[layer.variable]
        is_3d = var.extra_dim is not None
        self.widget_level.setVisible(is_3d)
        if not is_3d:
            return
        extra_dim = wrf_file.extra_dims[var.extra_dim]
        self.level_label.setText(f'{extra_dim.label}:')
        self.level_combo.blockSignals(True)
        self.level_combo.clear()
        for step_label in extra_dim.steps:
            self.level_combo.addItem(step_label)
        self.level_combo.setCurrentIndex(min(layer.level_index, self.level_combo.count() - 1))
        self.level_combo.blockSignals(False)

    @pyqtSlot(int)
    def on_variable_changed(self, index: int) -> None:
        layer = self._selected_layer()
        if layer is None or index < 0:
            return
        layer.variable = self.variable_combo.itemData(index)
        layer.level_index = 0
        # A unit choice belongs to the *old* variable - the new one may not
        # even share that unit key (units.find would raise) - so reset to
        # native. (Range/vmin,vmax is intentionally left alone, same as
        # before this change: a stale manual range on a new variable is
        # visibly odd rather than a crash, and "Auto" is one click away.)
        layer.units = None
        wrf_file = self._renderer.open_file(layer.file_path)
        var = wrf_file.variables[layer.variable]
        # Auto-detect with manual override: default a categorical variable
        # to the categorical colormap, and pull a layer back off it when the
        # new variable isn't categorical - _populate_colormap_combo only
        # ever offers "Categorical" for a categorical variable, so leaving a
        # stale CATEGORICAL selection here would silently fall back to
        # whatever ends up at index 0 there. Only fires on an actual
        # variable switch - the combo itself still lets the user manually
        # pick a continuous map for a categorical variable afterwards.
        if var.category_scheme is not None:
            layer.colormap = colormaps.CATEGORICAL
        elif layer.colormap == colormaps.CATEGORICAL:
            layer.colormap = 'viridis'
        self._populate_level_combo(wrf_file, layer)
        self._rebuild_layer_tree(select_id=layer.layer_id)
        self.refresh_map()

    @pyqtSlot(int)
    def on_time_changed(self, index: int) -> None:
        layer = self._selected_layer()
        if layer is None or index < 0:
            return
        layer.time_index = index
        self.refresh_map()

    @pyqtSlot()
    def on_prev_time_button_clicked(self) -> None:
        if self.time_combo.currentIndex() > 0:
            self.time_combo.setCurrentIndex(self.time_combo.currentIndex() - 1)

    @pyqtSlot()
    def on_next_time_button_clicked(self) -> None:
        if self.time_combo.currentIndex() < self.time_combo.count() - 1:
            self.time_combo.setCurrentIndex(self.time_combo.currentIndex() + 1)

    @pyqtSlot(bool)
    def on_play_button_toggled(self, checked: bool) -> None:
        self.play_button.setText('❚❚' if checked else '▶')
        if checked:
            self._play_timer.start()
        else:
            self._play_timer.stop()

    def _advance_play(self) -> None:
        if self.time_combo.count() == 0:
            return
        # Loops back to the start rather than stopping at the end - an
        # animation that just halts on the last frame reads as "it broke",
        # not "it finished".
        next_index = (self.time_combo.currentIndex() + 1) % self.time_combo.count()
        self.time_combo.setCurrentIndex(next_index)

    @pyqtSlot(int)
    def on_level_changed(self, index: int) -> None:
        layer = self._selected_layer()
        if layer is None or index < 0:
            return
        layer.level_index = index
        self.refresh_map()

    @pyqtSlot(int)
    def on_units_changed(self, index: int) -> None:
        layer = self._selected_layer()
        if layer is None or index < 0:
            return
        key = self.units_combo.itemData(index)
        new_units = None if key == 'native' else key
        if new_units != layer.units:
            self._rescale_manual_range(layer, new_units)
        layer.units = new_units
        self._rebuild_layer_tree(select_id=layer.layer_id)
        self.refresh_map()

    def _rescale_manual_range(self, layer: RasterLayer, new_units: Optional[str]) -> None:
        """Converts a manual vmin/vmax (stored in the layer's *previous*
        display unit) into the new one, so switching units keeps the same
        view instead of the range suddenly meaning something else (or, for
        an auto range, going blank until the next redraw recomputes it)."""
        if layer.vmin is None or layer.vmax is None:
            return
        wrf_file = self._renderer.open_file(layer.file_path)
        var = wrf_file.variables[layer.variable]
        old_unit = units_module.find(var.units, layer.units) if layer.units is not None \
            else units_module.Unit(key='native', label=var.units, scale=1.0, offset=0.0)
        new_unit = units_module.find(var.units, new_units) if new_units is not None \
            else units_module.Unit(key='native', label=var.units, scale=1.0, offset=0.0)
        native_vmin = (layer.vmin - old_unit.offset) / old_unit.scale
        native_vmax = (layer.vmax - old_unit.offset) / old_unit.scale
        rescaled_vmin = units_module.convert(native_vmin, new_unit)
        rescaled_vmax = units_module.convert(native_vmax, new_unit)
        layer.vmin, layer.vmax = sorted((rescaled_vmin, rescaled_vmax))

    @pyqtSlot(int)
    def on_colormap_changed(self, index: int) -> None:
        layer = self._selected_layer()
        if layer is None or index < 0:
            return
        layer.colormap = self.colormap_combo.itemData(index)
        self._rebuild_layer_tree(select_id=layer.layer_id)
        self.refresh_map()

    @pyqtSlot(int)
    def on_opacity_changed(self, value: int) -> None:
        layer = self._selected_layer()
        if layer is None:
            return
        layer.opacity = value / 100.0
        self.opacity_label.setText(f'{value}%')
        self.refresh_map()

    @pyqtSlot()
    def on_range_changed(self) -> None:
        layer = self._selected_layer()
        if layer is None:
            return
        auto = self.auto_range_check.isChecked()
        self.vmin.setEnabled(not auto)
        self.vmax.setEnabled(not auto)
        if auto:
            layer.vmin = None
            layer.vmax = None
        else:
            if not (self.vmin.is_valid() and self.vmax.is_valid()):
                return
            vmin, vmax = self.vmin.value(), self.vmax.value()
            if vmax <= vmin:
                raise UserError('The layer\'s maximum value must be greater than its minimum.')
            layer.vmin, layer.vmax = vmin, vmax
        self._rebuild_layer_tree(select_id=layer.layer_id)
        self.refresh_map()

    @pyqtSlot(bool)
    def on_interpolate_changed(self, checked: bool) -> None:
        layer = self._selected_layer()
        if layer is None:
            return
        layer.interpolate = checked
        self.refresh_map()

    # --- colorbar appearance -------------------------------------------------
    # Ticks/format/decimals are legend-only (see RasterLayer's docstring for
    # these fields) - these handlers update the colorbar directly rather
    # than going through refresh_map(): no re-render of the layer itself is
    # needed for a purely cosmetic legend change.

    @pyqtSlot(int)
    def on_tick_count_changed(self, value: int) -> None:
        layer = self._selected_layer()
        if layer is None:
            return
        layer.tick_count = value
        self._update_colorbar()

    @pyqtSlot(int)
    def on_tick_format_changed(self, index: int) -> None:
        layer = self._selected_layer()
        if layer is None or index < 0:
            return
        layer.tick_format = self.tick_format_combo.itemData(index)
        self.tick_decimals_spin.setEnabled(layer.tick_format != 'auto')
        self._update_colorbar()

    @pyqtSlot(int)
    def on_tick_decimals_changed(self, value: int) -> None:
        layer = self._selected_layer()
        if layer is None:
            return
        layer.tick_decimals = value
        self._update_colorbar()

    @pyqtSlot(bool)
    def on_show_info_toggled(self, checked: bool) -> None:
        self._update_colorbar()

    # --- view --------------------------------------------------------------

    @pyqtSlot()
    def on_zoom_to_layer_button_clicked(self) -> None:
        layer = self._selected_layer()
        if layer is None:
            return
        self._zoom_to_layer(layer)

    def _zoom_to_layer(self, layer: RasterLayer) -> None:
        overlay = self._renderer.overlay_for(layer)
        if overlay is None:
            return
        minx, miny, maxx, maxy = overlay.bounds_3857
        min_lon, min_lat = _mercator_to_lonlat(minx, miny)
        max_lon, max_lat = _mercator_to_lonlat(maxx, maxy)
        self.map_widget.fit_bounds(min_lon, min_lat, max_lon, max_lat)

    # --- map redraw ----------------------------------------------------------

    def refresh_map(self) -> None:
        overlays = []
        for layer in self._layers:  # bottom-first: draw order
            if not layer.visible:
                continue
            try:
                overlay = self._renderer.overlay_for(layer)
            except UserError:
                overlay = None
            if overlay is not None:
                overlays.append(overlay)
        self.map_widget.set_overlay_group('view-rasters', overlays, z=Z_RASTER)
        self._update_colorbar()

    def _update_colorbar(self) -> None:
        """Shows a colorbar for the selected layer (if visible and
        renderable) - a discrete swatch/label legend for the categorical
        colormap, otherwise a gradient matching its own colormap/range/unit/
        tick settings - hidden otherwise."""
        layer = self._selected_layer()
        if layer is None or not layer.visible:
            self.map_widget.set_legend(None)
            self._update_info_overlay(None, None, None)
            return

        wrf_file = self._renderer.open_file(layer.file_path)
        var = wrf_file.variables.get(layer.variable)
        title = layer.variable
        unit_label = self._display_unit_label(var, layer) if var else ''
        if unit_label:
            title += f' ({unit_label})'

        time_label = wrf_file.times[layer.time_index] if layer.time_index < len(wrf_file.times) else ''
        self._update_info_overlay(layer.variable, unit_label, time_label)

        if layer.colormap == colormaps.CATEGORICAL:
            try:
                legend = self._renderer.categorical_legend(layer)
            except UserError:
                legend = None
            if legend is None:
                self.map_widget.set_legend(None)
                return
            lut, labels, present = legend
            pixmap = colorbar.build_categorical_legend_pixmap(lut, labels, present, title)
            self.map_widget.set_legend(pixmap)
            return

        try:
            value_range = self._renderer.effective_range(layer)
        except UserError:
            value_range = None
        if value_range is None:
            self.map_widget.set_legend(None)
            return

        vmin, vmax = value_range
        pixmap = colorbar.build_legend_pixmap(
            layer.colormap, vmin, vmax, title,
            tick_count=layer.tick_count, tick_format=layer.tick_format, tick_decimals=layer.tick_decimals)
        self.map_widget.set_legend(pixmap)

    def _update_info_overlay(self, variable: Optional[str], unit_label: Optional[str], time_label: Optional[str]) -> None:
        """Drives the map's optional movable info-text overlay (see
        TileMapWidget.set_info_text) - hidden unless both "Show Info
        Overlay" is checked and there's an actual selected/visible layer to
        describe (the three args are only ever all-None or all-populated,
        from _update_colorbar's two call sites)."""
        if not self.show_info_check.isChecked() or variable is None:
            self.map_widget.set_info_text(None)
            return
        text = variable
        if unit_label:
            text += f' ({unit_label})'
        if time_label:
            text += f'  —  {time_label}'
        self.map_widget.set_info_text(text)

    @staticmethod
    def _display_unit_label(var: WRFVariable, layer: RasterLayer) -> str:
        if layer.units is None:
            return var.units
        return units_module.find(var.units, layer.units).label
