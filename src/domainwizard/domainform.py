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
"""

from math import ceil
from typing import Optional

from osgeo import osr
from PyQt6.QtCore import QMetaObject, Qt, pyqtSignal, pyqtSlot
from PyQt6.QtGui import QDoubleValidator, QIntValidator
from PyQt6.QtWidgets import (
    QWidget, QPushButton, QLayout, QVBoxLayout, QGridLayout, QGroupBox, QSpinBox,
    QLabel, QHBoxLayout, QComboBox, QFileDialog
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
    MyLineEdit, add_grid_lineedit, update_input_validation_style, create_lineedit,
    RATIO_VALIDATOR, DIM_VALIDATOR,
)

MAX_PARENTS = 22
DECIMALS = 50
# See the comment at its use in set_domain_to_extent().
MAX_REASONABLE_DIM = 5000
LON_VALIDATOR = QDoubleValidator(-180.0, 180.0, DECIMALS)
LAT_VALIDATOR = QDoubleValidator(-90.0, 90.0, DECIMALS)
RESOLUTION_VALIDATOR = QDoubleValidator(0.00000000000000000001, float('+inf'), DECIMALS)

HORIZONTAL_RESOLUTION_LABEL = 'Horizontal Resolution: {resolution} {unit}'


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

        # Import/Export
        import_from_namelist_button = QPushButton("Import from namelist")
        import_from_namelist_button.setObjectName('import_from_namelist_button')
        export_geogrid_namelist_button = QPushButton("Export to namelist")
        export_geogrid_namelist_button.setObjectName('export_geogrid_namelist_button')

        vbox_import_export = QVBoxLayout()
        vbox_import_export.addWidget(import_from_namelist_button)
        vbox_import_export.addWidget(export_geogrid_namelist_button)
        self.gbox_import_export = QGroupBox("Import/Export")
        self.gbox_import_export.setLayout(vbox_import_export)

        # Group: Map Type
        self.group_box_map_type = QGroupBox("Map Type")
        vbox_map_type = QVBoxLayout()
        hbox_map_type = QHBoxLayout()

        self.projection = QComboBox()
        self.projection.setObjectName('projection')
        projs = {
            'undefined': '-',
            'lat-lon': 'Latitude/Longitude',
            'lambert': 'Lambert Conformal',
            'mercator': 'Mercator',
            'polar': 'Polar Stereographic',
        }
        for proj_id, proj_label in projs.items():
            self.projection.addItem(proj_label, proj_id)

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

        # editingFinished (see the loop below) only fires once a field loses
        # focus, so the view only updates once every required field has been
        # tabbed/clicked away from in turn. This button lets the user force
        # a redraw immediately once they've filled in what's needed, without
        # depending on field-visit order.
        refresh_view_button = QPushButton("Refresh View")
        refresh_view_button.setObjectName('refresh_view_button')
        refresh_view_button.clicked.connect(lambda: self.on_change_any_field(zoom_out=True, raise_on_invalid=True))
        vbox_map_type.addWidget(refresh_view_button)

        self.group_box_map_type.setLayout(vbox_map_type)

        # Group: Horizontal Resolution
        self.group_box_resol = QGroupBox("Horizontal Grid Spacing")
        hbox_resol = QHBoxLayout()
        self.resolution = MyLineEdit(required=True)
        self.resolution.setValidator(RESOLUTION_VALIDATOR)
        self.resolution.textChanged.connect(lambda _: update_input_validation_style(self.resolution))
        self.resolution.textChanged.emit(self.resolution.text())
        hbox_resol.addWidget(self.resolution)
        self.resolution_label = QLabel()
        hbox_resol.addWidget(self.resolution_label)
        self.group_box_resol.setLayout(hbox_resol)

        # Group: Automatic Domain Generator
        self.group_box_auto_domain = QGroupBox("Grid Extent Calculator")
        vbox_auto_domain = QVBoxLayout()
        domain_pb_set_canvas_extent = QPushButton("Set to Map View Extent")
        domain_pb_set_canvas_extent.setObjectName('set_canvas_extent_button')
        domain_pb_set_file_extent = QPushButton("Set from File...")
        domain_pb_set_file_extent.setObjectName('set_file_extent_button')
        vbox_auto_domain.addWidget(domain_pb_set_canvas_extent)
        vbox_auto_domain.addWidget(domain_pb_set_file_extent)
        self.group_box_auto_domain.setLayout(vbox_auto_domain)

        # Group: Manual Domain Configuration
        grid_center_point = QGridLayout()
        self.center_lon = add_grid_lineedit(grid_center_point, 0, 'Longitude', LON_VALIDATOR, '°', required=True)
        self.center_lat = add_grid_lineedit(grid_center_point, 1, 'Latitude', LAT_VALIDATOR, '°', required=True)
        group_box_centre_point = QGroupBox("Center Point")
        group_box_centre_point.setLayout(grid_center_point)

        grid_dims = QGridLayout()
        self.cols = add_grid_lineedit(grid_dims, 0, 'Horizontal', DIM_VALIDATOR, required=True)
        self.rows = add_grid_lineedit(grid_dims, 1, 'Vertical', DIM_VALIDATOR, required=True)
        group_box_dims = QGroupBox("Grid Extent")
        group_box_dims.setLayout(grid_dims)

        vbox_manual_domain = QVBoxLayout()
        vbox_manual_domain.addWidget(group_box_centre_point)
        vbox_manual_domain.addWidget(group_box_dims)

        self.group_box_manual_domain = QGroupBox("Advanced Configuration")
        self.group_box_manual_domain.setCheckable(True)
        self.group_box_manual_domain.setChecked(False)
        self.group_box_manual_domain.setLayout(vbox_manual_domain)

        for field in [self.resolution, self.center_lat, self.center_lon, self.rows, self.cols,
                      self.truelat1, self.truelat2, self.stand_lon]:
            field.editingFinished.connect(self.on_change_any_field)

        # Group Box: Parent Domain
        self.group_box_parent_domain = QGroupBox("Enable Parenting")
        self.group_box_parent_domain.setObjectName('group_box_parent_domain')
        self.group_box_parent_domain.setCheckable(True)
        self.group_box_parent_domain.setChecked(False)

        hbox_parent_num = QHBoxLayout()
        hbox_parent_num.addWidget(QLabel('Number of Parent Domains:'))
        self.parent_spin = QSpinBox()
        self.parent_spin.setObjectName('parent_spin')
        self.parent_spin.setRange(1, MAX_PARENTS)
        hbox_parent_num.addWidget(self.parent_spin)
        self.group_box_parent_domain.setLayout(hbox_parent_num)

        self.parent_domains: list = []
        self.parent_vbox = QVBoxLayout()
        self.parent_vbox.setSizeConstraint(QLayout.SizeConstraint.SetMinimumSize)

        go_to_data_tab_btn = QPushButton('Continue to Datasets')
        go_to_data_tab_btn.clicked.connect(self.go_to_data_tab)

        dom_mgr_layout = QVBoxLayout()
        dom_mgr_layout.addWidget(self.gbox_import_export)
        dom_mgr_layout.addWidget(self.group_box_map_type)
        dom_mgr_layout.addWidget(self.group_box_resol)
        dom_mgr_layout.addWidget(self.group_box_auto_domain)
        dom_mgr_layout.addWidget(self.group_box_manual_domain)
        dom_mgr_layout.addWidget(self.group_box_parent_domain)
        dom_mgr_layout.addLayout(self.parent_vbox)
        dom_mgr_layout.addWidget(go_to_data_tab_btn)
        dom_mgr_layout.addStretch(1)
        self.setLayout(dom_mgr_layout)

        QMetaObject.connectSlotsByName(self)

        # trigger event for initial layout
        self.projection.currentIndexChanged.emit(self.projection.currentIndex())

    @property
    def project(self) -> Project:
        return self._project

    @project.setter
    def project(self, val: Project) -> None:
        self._project = val
        self.populate_ui_from_project()

    def populate_ui_from_project(self) -> None:
        project = self.project
        try:
            domains = project.data['domains']
        except KeyError:
            return

        main_domain = domains[0]
        map_proj = main_domain['map_proj']

        idx = self.projection.findData(map_proj)
        self.projection.setCurrentIndex(idx)

        if map_proj in ['lambert', 'mercator', 'polar']:
            self.truelat1.set_value(main_domain['truelat1'])
        if map_proj == 'lambert':
            self.truelat2.set_value(main_domain['truelat2'])
        if map_proj in ['lambert', 'polar']:
            self.stand_lon.set_value(main_domain['stand_lon'])

        self.resolution.set_value(main_domain['cell_size'][0])

        lon, lat = main_domain['center_lonlat']
        self.center_lat.set_value(lat)
        self.center_lon.set_value(lon)

        cols, rows = main_domain['domain_size']
        self.rows.set_value(rows)
        self.cols.set_value(cols)

        if len(domains) > 1:
            self.group_box_parent_domain.setChecked(True)
            self.parent_spin.setValue(len(domains) - 1)
            self.on_parent_spin_valueChanged(len(domains) - 1)

            for idx, parent_domain in enumerate(domains[1:]):
                fields, _ = self.parent_domains[idx]
                fields = fields['inputs']
                field_to_key = {
                    'ratio': 'parent_cell_size_ratio',
                    'top': 'padding_top',
                    'left': 'padding_left',
                    'right': 'padding_right',
                    'bottom': 'padding_bottom',
                }
                for field_name, key in field_to_key.items():
                    fields[field_name].set_value(parent_domain[key])

        self.draw_bbox_and_grids(zoom_out=True)

    @pyqtSlot()
    def on_import_from_namelist_button_clicked(self) -> None:
        file_path, _ = QFileDialog.getOpenFileName(self, caption='Open WPS namelist')
        if not file_path:
            return
        nml = read_namelist(file_path, schema_name='wps')
        self.project = convert_wps_nml_to_project(nml, self.project)

    @pyqtSlot()
    def on_export_geogrid_namelist_button_clicked(self):
        if not self.update_project():
            raise UserError('Domain configuration invalid, check fields')
        file_path, _ = QFileDialog.getSaveFileName(self, caption='Save WPS namelist as', directory='namelist.wps')
        if not file_path:
            return
        wps_namelist = convert_project_to_wps_namelist(self.project)
        write_namelist(wps_namelist, file_path)

    def create_domain_crs(self) -> CRS:
        proj = self.get_proj_kwargs()
        if proj is None:
            raise UserError('Incomplete projection definition')

        map_proj = proj['map_proj']
        if map_proj == 'lambert':
            origin_lat = self.center_lat.value() if self.center_lat.is_valid() else 0
            crs = CRS.create_lambert(proj['truelat1'], proj['truelat2'], LonLat(proj['stand_lon'], origin_lat))
        elif map_proj == 'polar':
            crs = CRS.create_polar(proj['truelat1'], proj['stand_lon'])
        elif map_proj == 'mercator':
            origin_lon = self.center_lon.value() if self.center_lon.is_valid() else 0
            crs = CRS.create_mercator(proj['truelat1'], origin_lon)
        elif map_proj == 'lat-lon':
            crs = CRS.create_lonlat()
        else:
            assert False, 'unknown proj: ' + map_proj
        return crs

    @pyqtSlot()
    def on_set_canvas_extent_button_clicked(self):
        if not self.resolution.is_valid():
            return
        min_lon, min_lat, max_lon, max_lat = self.map_widget.current_view_bbox()
        bbox = BoundingBox2D(minx=min_lon, maxx=max_lon, miny=min_lat, maxy=max_lat)
        self.set_domain_to_extent(_lonlat_srs(), bbox)

    @pyqtSlot()
    def on_set_file_extent_button_clicked(self):
        if not self.resolution.is_valid():
            return
        file_path, _ = QFileDialog.getOpenFileName(self, caption='Select a raster or vector file to read the extent from')
        if not file_path:
            return
        try:
            bbox, srs = read_extent_and_srs(file_path)
        except ValueError as e:
            raise UserError(str(e))
        self.set_domain_to_extent(srs, bbox)

    def set_domain_to_extent(self, srs: osr.SpatialReference, bbox: BoundingBox2D) -> None:
        resolution = self.resolution.value()

        extent_crs = CRS(srs=srs)
        domain_crs = self.create_domain_crs()
        domain_srs = domain_crs.srs

        domain_bbox = extent_crs.transform_bbox(bbox, domain_srs)

        xmin, xmax, ymin, ymax = domain_bbox.minx, domain_bbox.maxx, domain_bbox.miny, domain_bbox.maxy

        cols = ceil((xmax - xmin) / resolution)
        rows = ceil((ymax - ymin) / resolution)

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

        center_x = xmin + (xmax - xmin) / 2
        center_y = ymin + (ymax - ymin) / 2
        center_lonlat = domain_crs.to_lonlat(Coordinate2D(center_x, center_y))
        self.center_lat.set_value(center_lonlat.lat)
        self.center_lon.set_value(center_lonlat.lon)
        self.resolution.set_value(resolution)
        self.cols.set_value(cols)
        self.rows.set_value(rows)

        self.on_change_any_field(zoom_out=True)

    @pyqtSlot()
    def on_group_box_parent_domain_clicked(self):
        if self.group_box_parent_domain.isChecked():
            self.add_parent_domain()
        else:
            self.parent_spin.setValue(1)
            while self.parent_domains:
                self.remove_last_parent_domain()

    def add_parent_domain(self):
        idx = len(self.parent_domains) + 1
        fields, group_box_parent = create_parent_group_box('Parent ' + str(idx), '?', self.proj_res_unit, required=True)
        self.parent_vbox.addWidget(group_box_parent)
        group_box_parent.show()
        self.parent_domains.append((fields, group_box_parent))
        self.adjustSize()
        for field in fields['inputs'].values():
            field.editingFinished.connect(self.on_change_any_field)

    def remove_last_parent_domain(self):
        _, group_box_parent = self.parent_domains.pop()
        group_box_parent.deleteLater()
        self.parent_vbox.removeWidget(group_box_parent)
        self.on_change_any_field()

    @pyqtSlot(int)
    def on_parent_spin_valueChanged(self, value: int) -> None:
        count = len(self.parent_domains)
        for _ in range(value, count):
            self.remove_last_parent_domain()
        for _ in range(count, value):
            self.add_parent_domain()

    @pyqtSlot(int)
    def on_projection_currentIndexChanged(self, index: int) -> None:
        proj_id = self.projection.currentData()
        is_undefined = proj_id == 'undefined'
        is_lat_lon = proj_id == 'lat-lon'
        is_projected = not is_undefined and not is_lat_lon

        self.group_box_resol.setDisabled(is_undefined)
        self.group_box_auto_domain.setDisabled(is_undefined)
        self.group_box_manual_domain.setDisabled(is_undefined)
        self.group_box_parent_domain.setDisabled(is_undefined)

        def update_field(field, enabled):
            field.required = enabled
            field.setEnabled(enabled)
            if not enabled:
                field.setText('')
            field.textChanged.emit(field.text())

        self.widget_proj_params.setVisible(is_projected)
        update_field(self.truelat1, proj_id in ['lambert', 'mercator', 'polar'])
        update_field(self.truelat2, proj_id == 'lambert')
        update_field(self.stand_lon, proj_id in ['lambert', 'polar'])

        if is_undefined:
            self.proj_res_unit = ''
        elif is_lat_lon:
            self.proj_res_unit = '°'
        else:
            self.proj_res_unit = 'm'
        self.resolution_label.setText(self.proj_res_unit)

        self.group_box_parent_domain.setChecked(False)
        for _ in list(self.parent_domains):
            self.remove_last_parent_domain()

        self.adjustSize()

    def get_proj_kwargs(self) -> Optional[dict]:
        proj_id = self.projection.currentData()
        kwargs = {'map_proj': proj_id}
        if proj_id in ['lambert', 'mercator', 'polar']:
            if not self.truelat1.is_valid():
                return None
            kwargs['truelat1'] = self.truelat1.value()
        if proj_id == 'lambert':
            if not self.truelat2.is_valid():
                return None
            kwargs['truelat2'] = self.truelat2.value()
        if proj_id in ['lambert', 'polar']:
            if not self.stand_lon.is_valid():
                return None
            kwargs['stand_lon'] = self.stand_lon.value()
        return kwargs

    def update_project(self) -> bool:
        proj_kwargs = self.get_proj_kwargs()
        if proj_kwargs is None:
            return False

        valid = all(w.is_valid() for w in [self.center_lat, self.center_lon, self.resolution, self.cols, self.rows])
        if not valid:
            return False
        center_lonlat = LonLat(lon=self.center_lon.value(), lat=self.center_lat.value())
        resolution = self.resolution.value()
        domain_size = (self.cols.value(), self.rows.value())

        parent_domains = []
        for fields, _ in self.parent_domains:
            inputs = fields['inputs']
            valid = all(w.is_valid() for w in inputs.values())
            if not valid:
                return False
            ratio, top, left, right, bottom = [inputs[name].value() for name in ['ratio', 'top', 'left', 'right', 'bottom']]
            parent_domains.append({
                'parent_cell_size_ratio': ratio,
                'padding_left': left,
                'padding_right': right,
                'padding_bottom': bottom,
                'padding_top': top,
            })

        self.project.set_domains(
            cell_size=(resolution, resolution), domain_size=domain_size,
            center_lonlat=center_lonlat, parent_domains=parent_domains, **proj_kwargs)
        return True

    def on_change_any_field(self, zoom_out=False, raise_on_invalid=False):
        if not self.update_project():
            if raise_on_invalid:
                raise UserError(
                    'Domain configuration invalid or incomplete - check the highlighted fields '
                    '(red = invalid, yellow = required but empty), including any parent domains.')
            return

        domains = self.project.data['domains']
        main_domain_size = domains[0]['domain_size']
        self.cols.set_value(main_domain_size[0])
        self.rows.set_value(main_domain_size[1])

        for (fields, _), domain in zip(self.parent_domains, domains[1:]):
            res_label = fields['other']['resolution']
            res_label.setText(HORIZONTAL_RESOLUTION_LABEL.format(resolution=domain['cell_size'][0], unit=self.proj_res_unit))
            for name in ['left', 'right', 'top', 'bottom']:
                fields['inputs'][name].set_value(domain['padding_' + name])

        self.draw_bbox_and_grids(zoom_out)

    def draw_bbox_and_grids(self, zoom_out: bool) -> None:
        project = self.project
        try:
            overlays = compute_domain_overlays(project)
        except UserError:
            return

        self.map_widget.set_overlays(overlays)
        if zoom_out:
            bounds = domain_lonlat_bounds(project)
            if bounds is not None:
                self.map_widget.fit_bounds(*bounds)


def create_parent_group_box(name: str, res, unit: str, required: bool = False) -> tuple:
    """Returns a 'validator-ready' group box to be used by the parent-domain tab."""
    parent_child_ratio_box = QGridLayout()
    parent_child_ratio = add_grid_lineedit(parent_child_ratio_box, 0, 'Child-to-Parent Ratio', RATIO_VALIDATOR, required=required)

    res_label = QLabel(HORIZONTAL_RESOLUTION_LABEL.format(resolution=res, unit=unit))

    sub_group_box = QGroupBox("Padding")
    grid = QGridLayout()
    top_label = QLabel('Top')
    top_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    grid.addWidget(top_label, 0, 1)
    left_label = QLabel('Left')
    left_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    grid.addWidget(left_label, 2, 0)
    right_label = QLabel('Right')
    right_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    grid.addWidget(right_label, 2, 2)
    bottom_label = QLabel('Bottom')
    bottom_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    grid.addWidget(bottom_label, 4, 1)
    top = create_lineedit(DIM_VALIDATOR, required)
    left = create_lineedit(DIM_VALIDATOR, required)
    right = create_lineedit(DIM_VALIDATOR, required)
    bottom = create_lineedit(DIM_VALIDATOR, required)
    grid.addWidget(top, 1, 1)
    grid.addWidget(left, 3, 0)
    grid.addWidget(right, 3, 2)
    grid.addWidget(bottom, 5, 1)
    sub_group_box.setLayout(grid)

    vbox = QVBoxLayout()
    vbox.addLayout(parent_child_ratio_box)
    vbox.addWidget(res_label)
    vbox.addWidget(sub_group_box)
    group_box = QGroupBox(name)
    group_box.setLayout(vbox)
    return {
        'inputs': {'ratio': parent_child_ratio, 'top': top, 'left': left, 'right': right, 'bottom': bottom},
        'other': {'resolution': res_label},
    }, group_box
