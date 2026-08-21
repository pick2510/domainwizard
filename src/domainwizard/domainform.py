"""Domain configuration wizard, ported from gis4wrf/plugin/ui/widget_domains.py.

Differences from the QGIS-plugin version:
- Takes a TileMapWidget instead of a QgisInterface `iface`.
- No "Set Map CRS" button: that only existed to sync QGIS's project CRS for
  on-the-fly reprojection: TileMapWidget always projects overlays to lon/lat
  itself on every repaint, so there's nothing to sync.
- "Set to Active Layer Extent" -> "Set from File": there's no QGIS layer list
  to borrow an extent from outside QGIS, so the user picks a file directly
  and its extent/CRS are read with GDAL/OGR (fileextent.py).
- No Broadcast signal bus: this widget owns its Project directly, so
  "Import from namelist" just replaces self.project instead of emitting a
  cross-widget signal.

Phase 2 of PLAN_TREE_DOMAINS.md: unlike the QGIS plugin (and this port's own
Phase 1), the domain editor here is a tree (QTreeWidget), not a fixed chain
of "Parent N" boxes - domains can share a parent (siblings), matching what
gis4wrf.core's WPS-native storage (root-first, explicit parent_id per
domain) has supported since Phase 1. There's no more "update_project()
rebuilds everything from a flat field list" - each field edit writes
directly into the currently *selected* domain's dict in
self.project.data['domains'] and calls fill_domains() to recompute.
"""

from math import ceil
from typing import Optional

from osgeo import osr
from PyQt6.QtCore import Qt, pyqtSignal, pyqtSlot
from PyQt6.QtGui import QDoubleValidator
from PyQt6.QtWidgets import (
    QWidget, QPushButton, QVBoxLayout, QGridLayout, QGroupBox,
    QLabel, QHBoxLayout, QComboBox, QFileDialog, QTreeWidget, QTreeWidgetItem, QMessageBox
)

from gis4wrf.core import (
    LonLat, Coordinate2D, CRS, BoundingBox2D, Project, read_namelist, write_namelist,
    convert_wps_nml_to_project, convert_project_to_wps_namelist,
    UserError,
)

from domainwizard.tilemap import TileMapWidget
from domainwizard.domainoverlay import compute_domain_overlays, domain_lonlat_bounds
from domainwizard.fileextent import read_extent_and_srs
from domainwizard.formhelpers import (
    MyLineEdit, add_grid_lineedit, update_input_validation_style,
    RATIO_VALIDATOR, DIM_VALIDATOR,
)

DECIMALS = 50
# See the comment at its use in set_domain_to_extent().
MAX_REASONABLE_DIM = 5000
LON_VALIDATOR = QDoubleValidator(-180.0, 180.0, DECIMALS)
LAT_VALIDATOR = QDoubleValidator(-90.0, 90.0, DECIMALS)
RESOLUTION_VALIDATOR = QDoubleValidator(0.00000000000000000001, float('+inf'), DECIMALS)

PROJECTIONS = {
    'undefined': '-',
    'lat-lon': 'Latitude/Longitude',
    'lambert': 'Lambert Conformal',
    'mercator': 'Mercator',
    'polar': 'Polar Stereographic',
}

# Qt.ItemDataRole.UserRole on each QTreeWidgetItem holds the domain's 1-based
# WPS domain number (== its position in project.data['domains']).
DOMAIN_NUMBER_ROLE = Qt.ItemDataRole.UserRole


def _lonlat_srs() -> osr.SpatialReference:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return srs


class DomainForm(QWidget):
    go_to_data_tab = pyqtSignal()

    def __init__(self, map_widget: TileMapWidget) -> None:
        super().__init__()
        self.map_widget = map_widget
        self._project = Project.create()
        self._selected_domain_number: Optional[int] = None

        # Import/Export
        import_from_namelist_button = QPushButton("Import from namelist")
        export_geogrid_namelist_button = QPushButton("Export to namelist")
        import_from_namelist_button.clicked.connect(self.on_import_from_namelist_button_clicked)
        export_geogrid_namelist_button.clicked.connect(self.on_export_geogrid_namelist_button_clicked)
        vbox_import_export = QVBoxLayout()
        vbox_import_export.addWidget(import_from_namelist_button)
        vbox_import_export.addWidget(export_geogrid_namelist_button)
        self.gbox_import_export = QGroupBox("Import/Export")
        self.gbox_import_export.setLayout(vbox_import_export)

        # Domain tree
        self.domain_tree = QTreeWidget()
        self.domain_tree.setHeaderHidden(True)
        self.domain_tree.itemSelectionChanged.connect(self.on_tree_selection_changed)
        self.add_domain_button = QPushButton("Add Root Domain")
        self.add_domain_button.clicked.connect(self.on_add_domain_button_clicked)
        self.remove_domain_button = QPushButton("Remove Domain")
        self.remove_domain_button.clicked.connect(self.on_remove_domain_button_clicked)
        self.remove_domain_button.setEnabled(False)
        vbox_tree = QVBoxLayout()
        vbox_tree.addWidget(self.domain_tree)
        vbox_tree.addWidget(self.add_domain_button)
        vbox_tree.addWidget(self.remove_domain_button)
        self.gbox_tree = QGroupBox("Domains")
        self.gbox_tree.setLayout(vbox_tree)

        # Map Type (root domain only - WPS defines one projection per
        # project regardless of nesting shape)
        self.gbox_map_type = QGroupBox("Map Type")
        vbox_map_type = QVBoxLayout()
        hbox_map_type = QHBoxLayout()
        self.projection = QComboBox()
        for proj_id, proj_label in PROJECTIONS.items():
            self.projection.addItem(proj_label, proj_id)
        self.projection.currentIndexChanged.connect(self.on_projection_changed)
        hbox_map_type.addWidget(QLabel('GCS/Projection:'))
        hbox_map_type.addWidget(self.projection)
        vbox_map_type.addLayout(hbox_map_type)

        proj_params_grid = QGridLayout()
        self.truelat1 = add_grid_lineedit(proj_params_grid, 0, 'True Latitude 1', LAT_VALIDATOR, unit='°', required=True)
        self.truelat2 = add_grid_lineedit(proj_params_grid, 1, 'True Latitude 2', LAT_VALIDATOR, unit='°', required=True)
        self.stand_lon = add_grid_lineedit(proj_params_grid, 2, 'Standard Longitude', LON_VALIDATOR, unit='°', required=True)
        self.widget_proj_params = QWidget()
        self.widget_proj_params.setLayout(proj_params_grid)
        vbox_map_type.addWidget(self.widget_proj_params)
        self.gbox_map_type.setLayout(vbox_map_type)

        # Horizontal Resolution (root only - descendants derive theirs from
        # their own parent_cell_size_ratio instead)
        self.gbox_resolution = QGroupBox("Horizontal Grid Spacing")
        hbox_resol = QHBoxLayout()
        self.resolution = MyLineEdit(required=True)
        self.resolution.setValidator(RESOLUTION_VALIDATOR)
        self.resolution.textChanged.connect(lambda _: update_input_validation_style(self.resolution))
        self.resolution.textChanged.emit(self.resolution.text())
        hbox_resol.addWidget(self.resolution)
        self.resolution_unit_label = QLabel()
        hbox_resol.addWidget(self.resolution_unit_label)
        self.gbox_resolution.setLayout(hbox_resol)

        # Nesting (non-root only)
        self.gbox_nesting = QGroupBox("Nesting")
        grid_nesting = QGridLayout()
        self.ratio = add_grid_lineedit(grid_nesting, 0, 'Child-to-Parent Ratio', RATIO_VALIDATOR, required=True)
        self.nesting_resolution_label = QLabel()
        grid_nesting.addWidget(self.nesting_resolution_label, 1, 0, 1, 3)
        self.gbox_nesting.setLayout(grid_nesting)

        # Grid Extent Calculator (both - meaning differs: for the root it
        # sets center+size directly, for a nested domain it sets this
        # domain's position within its parent (i_parent_start/j_parent_start
        # equivalent) + size)
        self.gbox_extent_calc = QGroupBox("Grid Extent Calculator")
        vbox_extent_calc = QVBoxLayout()
        set_canvas_extent_button = QPushButton("Set to Map View Extent")
        set_canvas_extent_button.clicked.connect(self.on_set_canvas_extent_button_clicked)
        set_file_extent_button = QPushButton("Set from File...")
        set_file_extent_button.clicked.connect(self.on_set_file_extent_button_clicked)
        vbox_extent_calc.addWidget(set_canvas_extent_button)
        vbox_extent_calc.addWidget(set_file_extent_button)
        self.gbox_extent_calc.setLayout(vbox_extent_calc)

        # Center Point (root only)
        self.gbox_center = QGroupBox("Center Point")
        grid_center = QGridLayout()
        self.center_lon = add_grid_lineedit(grid_center, 0, 'Longitude', LON_VALIDATOR, '°', required=True)
        self.center_lat = add_grid_lineedit(grid_center, 1, 'Latitude', LAT_VALIDATOR, '°', required=True)
        self.gbox_center.setLayout(grid_center)

        # Position within Parent (non-root only) - WPS's
        # i_parent_start/j_parent_start, 0-based here (padding_left/bottom)
        self.gbox_position = QGroupBox("Position within Parent")
        grid_position = QGridLayout()
        self.padding_left = add_grid_lineedit(grid_position, 0, 'From left edge', DIM_VALIDATOR, 'parent cells', required=True)
        self.padding_bottom = add_grid_lineedit(grid_position, 1, 'From bottom edge', DIM_VALIDATOR, 'parent cells', required=True)
        self.gbox_position.setLayout(grid_position)

        # Grid Extent (both)
        self.gbox_grid_extent = QGroupBox("Grid Extent")
        grid_extent = QGridLayout()
        self.cols = add_grid_lineedit(grid_extent, 0, 'Horizontal', DIM_VALIDATOR, required=True)
        self.rows = add_grid_lineedit(grid_extent, 1, 'Vertical', DIM_VALIDATOR, required=True)
        self.gbox_grid_extent.setLayout(grid_extent)

        for field in [self.truelat1, self.truelat2, self.stand_lon, self.resolution,
                      self.center_lon, self.center_lat, self.ratio,
                      self.padding_left, self.padding_bottom, self.cols, self.rows]:
            field.editingFinished.connect(self.on_selected_domain_field_changed)

        go_to_data_tab_btn = QPushButton('Continue to Datasets')
        go_to_data_tab_btn.clicked.connect(self.go_to_data_tab)

        dom_mgr_layout = QVBoxLayout()
        dom_mgr_layout.addWidget(self.gbox_import_export)
        dom_mgr_layout.addWidget(self.gbox_tree)
        dom_mgr_layout.addWidget(self.gbox_map_type)
        dom_mgr_layout.addWidget(self.gbox_resolution)
        dom_mgr_layout.addWidget(self.gbox_nesting)
        dom_mgr_layout.addWidget(self.gbox_extent_calc)
        dom_mgr_layout.addWidget(self.gbox_center)
        dom_mgr_layout.addWidget(self.gbox_position)
        dom_mgr_layout.addWidget(self.gbox_grid_extent)
        dom_mgr_layout.addWidget(go_to_data_tab_btn)
        dom_mgr_layout.addStretch(1)
        self.setLayout(dom_mgr_layout)

        self._update_panel_visibility()

    @property
    def project(self) -> Project:
        return self._project

    @project.setter
    def project(self, val: Project) -> None:
        self._project = val
        self._selected_domain_number = None
        self._rebuild_tree(select_number=1 if val.data.get('domains') else None)

    # --- domain tree -----------------------------------------------------

    def _rebuild_tree(self, select_number: Optional[int] = None) -> None:
        self.domain_tree.blockSignals(True)
        self.domain_tree.clear()
        domains = self._project.data.get('domains') or []

        items_by_number = {}
        for i, domain in enumerate(domains):
            number = i + 1
            item = QTreeWidgetItem([f'Domain {number}'])
            item.setData(0, DOMAIN_NUMBER_ROLE, number)
            items_by_number[number] = item

        for i, domain in enumerate(domains):
            number = i + 1
            if number == 1:
                self.domain_tree.addTopLevelItem(items_by_number[number])
            else:
                items_by_number[domain['parent_id']].addChild(items_by_number[number])

        self.domain_tree.expandAll()
        self.domain_tree.blockSignals(False)

        if select_number is not None and select_number in items_by_number:
            items_by_number[select_number].setSelected(True)
            self._selected_domain_number = select_number
        else:
            self._selected_domain_number = None
        self._update_panel_visibility()
        self._populate_properties_panel()
        self.draw_bbox_and_grids(zoom_out=True)

    @pyqtSlot()
    def on_tree_selection_changed(self) -> None:
        selected = self.domain_tree.selectedItems()
        self._selected_domain_number = selected[0].data(0, DOMAIN_NUMBER_ROLE) if selected else None
        self._update_panel_visibility()
        self._populate_properties_panel()

    def _selected_domain(self) -> Optional[dict]:
        if self._selected_domain_number is None:
            return None
        domains = self._project.data.get('domains') or []
        if self._selected_domain_number > len(domains):
            return None
        return domains[self._selected_domain_number - 1]

    def _is_root_selected(self) -> bool:
        return self._selected_domain_number == 1

    def _update_panel_visibility(self) -> None:
        has_selection = self._selected_domain_number is not None
        is_root = self._is_root_selected()

        self.gbox_map_type.setVisible(has_selection and is_root)
        self.gbox_resolution.setVisible(has_selection and is_root)
        self.gbox_center.setVisible(has_selection and is_root)
        self.gbox_nesting.setVisible(has_selection and not is_root)
        self.gbox_position.setVisible(has_selection and not is_root)
        self.gbox_extent_calc.setVisible(has_selection)
        self.gbox_grid_extent.setVisible(has_selection)

        has_domains = bool(self._project.data.get('domains'))
        self.remove_domain_button.setEnabled(has_selection)
        self.add_domain_button.setText('Add Root Domain' if not has_domains else 'Add Child Domain')
        # No domains yet: nothing to select, so the button (which creates the
        # root) is always enabled. Once domains exist, adding one needs a
        # selected parent.
        self.add_domain_button.setEnabled(has_selection or not has_domains)

    def _populate_properties_panel(self) -> None:
        domain = self._selected_domain()
        if domain is None:
            return

        if self._is_root_selected():
            map_proj = domain['map_proj']
            self.projection.blockSignals(True)
            self.projection.setCurrentIndex(self.projection.findData(map_proj))
            self.projection.blockSignals(False)
            self._update_proj_param_visibility(map_proj)
            if map_proj in ('lambert', 'mercator', 'polar'):
                self.truelat1.set_value(domain['truelat1'])
            if map_proj == 'lambert':
                self.truelat2.set_value(domain['truelat2'])
            if map_proj in ('lambert', 'polar'):
                self.stand_lon.set_value(domain['stand_lon'])
            self.resolution.set_value(domain['cell_size'][0])
            lon, lat = domain['center_lonlat']
            self.center_lon.set_value(lon)
            self.center_lat.set_value(lat)
        else:
            self.ratio.set_value(domain['parent_cell_size_ratio'])
            self.padding_left.set_value(domain['padding_left'])
            self.padding_bottom.set_value(domain['padding_bottom'])
            if 'cell_size' in domain:
                self.nesting_resolution_label.setText(
                    f"Resolution: {domain['cell_size'][0]:g} {self._resolution_unit()}")
            else:
                self.nesting_resolution_label.setText('')

        cols, rows = domain['domain_size']
        self.cols.set_value(cols)
        self.rows.set_value(rows)

    def _resolution_unit(self) -> str:
        root = self._project.data.get('domains', [None])[0]
        if root is None:
            return ''
        return '°' if root['map_proj'] == 'lat-lon' else 'm'

    def _update_proj_param_visibility(self, proj_id: str) -> None:
        is_projected = proj_id not in ('undefined', 'lat-lon')

        def update_field(field, enabled):
            field.required = enabled
            field.setEnabled(enabled)
            if not enabled:
                field.setText('')
            field.textChanged.emit(field.text())

        self.widget_proj_params.setVisible(is_projected)
        update_field(self.truelat1, proj_id in ('lambert', 'mercator', 'polar'))
        update_field(self.truelat2, proj_id == 'lambert')
        update_field(self.stand_lon, proj_id in ('lambert', 'polar'))
        self.resolution_unit_label.setText('°' if proj_id == 'lat-lon' else 'm' if is_projected else '')

    @pyqtSlot(int)
    def on_projection_changed(self, index: int) -> None:
        if not self._is_root_selected():
            return
        self._update_proj_param_visibility(self.projection.currentData())

    # --- add/remove domains -----------------------------------------------

    @pyqtSlot()
    def on_add_domain_button_clicked(self) -> None:
        domains = self._project.data.setdefault('domains', [])
        if not domains:
            domains.append({
                'map_proj': 'lat-lon',
                'parent_id': 1,
                'cell_size': [0.1, 0.1],
                'domain_size': [10, 10],
                'center_lonlat': [0.0, 0.0],
            })
            self._rebuild_tree(select_number=1)
            return

        if self._selected_domain_number is None:
            return
        parent_number = self._selected_domain_number
        parent = domains[parent_number - 1]
        domains.append({
            'parent_id': parent_number,
            'parent_cell_size_ratio': 3,
            'padding_left': 0,
            'padding_bottom': 0,
            'domain_size': [10, 10],
        })
        new_number = len(domains)
        try:
            self._project.fill_domains()
        except (UserError, KeyError):
            pass
        self._rebuild_tree(select_number=new_number)

    @pyqtSlot()
    def on_remove_domain_button_clicked(self) -> None:
        if self._selected_domain_number is None:
            return
        domains = self._project.data.get('domains') or []

        children_of = {}
        for i, domain in enumerate(domains):
            number = i + 1
            if number == 1:
                continue
            children_of.setdefault(domain['parent_id'], []).append(number)

        to_remove = set()
        stack = [self._selected_domain_number]
        while stack:
            number = stack.pop()
            to_remove.add(number)
            stack.extend(children_of.get(number, []))

        if len(to_remove) > 1:
            answer = QMessageBox.question(
                self, 'Remove Domain',
                f'Domain {self._selected_domain_number} has {len(to_remove) - 1} nested domain(s). '
                'Removing it will remove all of them too. Continue?',
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return

        surviving = [(i + 1, domain) for i, domain in enumerate(domains) if (i + 1) not in to_remove]
        old_to_new = {old_number: new_index + 1 for new_index, (old_number, _) in enumerate(surviving)}
        new_domains = []
        for old_number, domain in surviving:
            domain = dict(domain)
            if old_number != 1:
                domain['parent_id'] = old_to_new[domain['parent_id']]
            new_domains.append(domain)

        self._project.data['domains'] = new_domains
        self._rebuild_tree(select_number=old_to_new.get(1) if new_domains else None)

    # --- field edits -------------------------------------------------------

    @pyqtSlot()
    def on_selected_domain_field_changed(self) -> None:
        self._apply_selected_domain_fields(raise_on_invalid=False)

    def _apply_selected_domain_fields(self, raise_on_invalid: bool) -> bool:
        domain = self._selected_domain()
        if domain is None:
            return False

        if self._is_root_selected():
            ok = self._apply_root_fields(domain)
        else:
            ok = self._apply_nested_fields(domain)

        if not ok:
            if raise_on_invalid:
                raise UserError(
                    'Domain configuration invalid or incomplete - check the highlighted fields '
                    '(red = invalid, yellow = required but empty).')
            return False

        try:
            self._project.fill_domains()
        except UserError as e:
            if raise_on_invalid:
                raise
            return False

        self._populate_properties_panel()
        self.draw_bbox_and_grids(zoom_out=False)
        return True

    def _apply_root_fields(self, domain: dict) -> bool:
        proj_id = self.projection.currentData()
        if proj_id == 'undefined':
            return False

        fields_to_check = [self.resolution, self.center_lon, self.center_lat, self.cols, self.rows]
        if proj_id in ('lambert', 'mercator', 'polar'):
            fields_to_check.append(self.truelat1)
        if proj_id == 'lambert':
            fields_to_check.append(self.truelat2)
        if proj_id in ('lambert', 'polar'):
            fields_to_check.append(self.stand_lon)
        if not all(f.is_valid() for f in fields_to_check):
            return False

        domain['map_proj'] = proj_id
        if proj_id in ('lambert', 'mercator', 'polar'):
            domain['truelat1'] = self.truelat1.value()
        if proj_id == 'lambert':
            domain['truelat2'] = self.truelat2.value()
        if proj_id == 'lat-lon':
            domain['stand_lon'] = 0.0
        if proj_id in ('lambert', 'polar'):
            domain['stand_lon'] = self.stand_lon.value()

        resolution = self.resolution.value()
        domain['cell_size'] = [resolution, resolution]
        domain['center_lonlat'] = [self.center_lon.value(), self.center_lat.value()]
        domain['domain_size'] = [self.cols.value(), self.rows.value()]
        return True

    def _apply_nested_fields(self, domain: dict) -> bool:
        if not all(f.is_valid() for f in [self.ratio, self.padding_left, self.padding_bottom, self.cols, self.rows]):
            return False
        domain['parent_cell_size_ratio'] = self.ratio.value()
        domain['padding_left'] = self.padding_left.value()
        domain['padding_bottom'] = self.padding_bottom.value()
        domain['domain_size'] = [self.cols.value(), self.rows.value()]
        return True

    # --- namelist import/export --------------------------------------------

    @pyqtSlot()
    def on_import_from_namelist_button_clicked(self) -> None:
        file_path, _ = QFileDialog.getOpenFileName(self, caption='Open WPS namelist')
        if not file_path:
            return
        nml = read_namelist(file_path, schema_name='wps')
        self.project = convert_wps_nml_to_project(nml, self.project)

    @pyqtSlot()
    def on_export_geogrid_namelist_button_clicked(self) -> None:
        if not self._project.data.get('domains'):
            raise UserError('No domains configured yet')
        try:
            self._project.fill_domains()
        except UserError:
            raise UserError('Domain configuration invalid, check fields')
        file_path, _ = QFileDialog.getSaveFileName(self, caption='Save WPS namelist as', directory='namelist.wps')
        if not file_path:
            return
        wps_namelist = convert_project_to_wps_namelist(self._project)
        write_namelist(wps_namelist, file_path)

    # --- grid extent calculator --------------------------------------------

    @pyqtSlot()
    def on_set_canvas_extent_button_clicked(self) -> None:
        min_lon, min_lat, max_lon, max_lat = self.map_widget.current_view_bbox()
        bbox = BoundingBox2D(minx=min_lon, maxx=max_lon, miny=min_lat, maxy=max_lat)
        self.set_domain_to_extent(_lonlat_srs(), bbox)

    @pyqtSlot()
    def on_set_file_extent_button_clicked(self) -> None:
        file_path, _ = QFileDialog.getOpenFileName(self, caption='Select a raster or vector file to read the extent from')
        if not file_path:
            return
        try:
            bbox, srs = read_extent_and_srs(file_path)
        except ValueError as e:
            raise UserError(str(e))
        self.set_domain_to_extent(srs, bbox)

    def set_domain_to_extent(self, srs: osr.SpatialReference, bbox: BoundingBox2D) -> None:
        domain = self._selected_domain()
        if domain is None:
            return
        extent_crs = CRS(srs=srs)

        if self._is_root_selected():
            if not self.resolution.is_valid():
                return
            resolution = self.resolution.value()
            domain_crs = self._create_root_crs_from_fields()
            if domain_crs is None:
                raise UserError('Incomplete projection definition')
            domain_bbox = extent_crs.transform_bbox(bbox, domain_crs.srs)
            cols = ceil((domain_bbox.maxx - domain_bbox.minx) / resolution)
            rows = ceil((domain_bbox.maxy - domain_bbox.miny) / resolution)
            self._check_reasonable_size(cols, rows)
            center_x = domain_bbox.minx + (domain_bbox.maxx - domain_bbox.minx) / 2
            center_y = domain_bbox.miny + (domain_bbox.maxy - domain_bbox.miny) / 2
            center_lonlat = domain_crs.to_lonlat(Coordinate2D(center_x, center_y))
            self.center_lon.set_value(center_lonlat.lon)
            self.center_lat.set_value(center_lonlat.lat)
            self.cols.set_value(cols)
            self.rows.set_value(rows)
        else:
            if not self.ratio.is_valid():
                return
            try:
                self._project.fill_domains()
            except UserError as e:
                raise UserError(f'Configure the parent domain first: {e}')
            parent = self._project.data['domains'][domain['parent_id'] - 1]
            if 'bbox' not in parent:
                raise UserError('Configure the parent domain first')
            ratio = self.ratio.value()
            own_cell_size = [parent['cell_size'][0] / ratio, parent['cell_size'][1] / ratio]
            domain_crs = self._project.projection
            domain_bbox = extent_crs.transform_bbox(bbox, domain_crs.srs)
            cols = ceil((domain_bbox.maxx - domain_bbox.minx) / own_cell_size[0])
            rows = ceil((domain_bbox.maxy - domain_bbox.miny) / own_cell_size[1])
            self._check_reasonable_size(cols, rows)
            padding_left = round((domain_bbox.minx - parent['bbox'].minx) / parent['cell_size'][0])
            padding_bottom = round((domain_bbox.miny - parent['bbox'].miny) / parent['cell_size'][1])
            self.padding_left.set_value(padding_left)
            self.padding_bottom.set_value(padding_bottom)
            self.cols.set_value(cols)
            self.rows.set_value(rows)

        self._apply_selected_domain_fields(raise_on_invalid=True)
        self.draw_bbox_and_grids(zoom_out=True)

    def _check_reasonable_size(self, cols: int, rows: int) -> None:
        # Guards against e.g. clicking "Set to Map View Extent" while the map
        # is still zoomed out to (close to) the whole world: the resulting
        # domain is thousands of km wide, which real WRF domains never are,
        # and - for a regional projection like Lambert Conformal, whose
        # conformality breaks down far from its standard parallels - can
        # produce a self-intersecting, visually nonsensical outline (seen in
        # practice: a domain spanning nearly all longitudes wrapped across
        # the +/-180 degree antimeridian, which TileMapWidget's overlay
        # drawing has no wraparound handling for). No real WRF domain needs
        # anywhere near this many cells per side.
        if cols > MAX_REASONABLE_DIM or rows > MAX_REASONABLE_DIM:
            raise UserError(
                f'The computed domain is {cols} x {rows} cells, which is too large to be a '
                'real WRF domain. This usually means the map view (or the file used for '
                '"Set from File") covers a much larger area than intended - zoom in on the map '
                'first, or increase the horizontal grid spacing, then try again.')

    def _create_root_crs_from_fields(self) -> Optional[CRS]:
        proj_id = self.projection.currentData()
        if proj_id == 'lambert':
            if not (self.truelat1.is_valid() and self.truelat2.is_valid() and self.stand_lon.is_valid()):
                return None
            origin_lat = self.center_lat.value() if self.center_lat.is_valid() else 0
            return CRS.create_lambert(self.truelat1.value(), self.truelat2.value(), LonLat(self.stand_lon.value(), origin_lat))
        elif proj_id == 'polar':
            if not (self.truelat1.is_valid() and self.stand_lon.is_valid()):
                return None
            return CRS.create_polar(self.truelat1.value(), self.stand_lon.value())
        elif proj_id == 'mercator':
            if not self.truelat1.is_valid():
                return None
            origin_lon = self.center_lon.value() if self.center_lon.is_valid() else 0
            return CRS.create_mercator(self.truelat1.value(), origin_lon)
        elif proj_id == 'lat-lon':
            return CRS.create_lonlat()
        return None

    # --- map redraw ----------------------------------------------------------

    def draw_bbox_and_grids(self, zoom_out: bool) -> None:
        project = self._project
        if not project.data.get('domains'):
            self.map_widget.set_overlays([])
            return
        try:
            overlays = compute_domain_overlays(project)
        except UserError:
            self.map_widget.set_overlays([])
            return

        self.map_widget.set_overlays(overlays)
        if zoom_out:
            bounds = domain_lonlat_bounds(project)
            if bounds is not None:
                self.map_widget.fit_bounds(*bounds)
