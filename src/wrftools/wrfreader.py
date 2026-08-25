"""GDAL-only access to WRF/WPS NetCDF files (geo_em*, met_em*, wrfinput*,
wrfout*) for the View tab (see the View-tab plan). Deliberately does not use
the `netCDF4` package the way gis4wrf.core's (unvendored)
`wrf_netcdf_to_gdal.py` does - this app has no netCDF4 dependency, and GDAL's
own netCDF driver turns out to be sufficient for everything needed here.

Key differences from that reference, all verified by hand against real files
before writing this module (see the View-tab plan for the numbers):

- CRS: built with gis4wrf.core.CRS, which (as of the datum fix elsewhere in
  this plan) already uses the WRF sphere, not WGS84.
- Geotransform: derived from the staggered XLONG_U/XLAT_U and XLONG_V/XLAT_V
  coordinate arrays (edge-based), not the mass grid (cell-centered) - using
  the mass grid would offset every raster by half a cell. Read via the
  classic API by name (`NETCDF:"path":XLONG_U`) rather than the subdataset
  list (which hides coordinate variables) or the multidim API (which cannot
  read them at all on files with an unlimited Time dimension - confirmed on
  wrfout/wrfinput fixtures, `arrayStartIdx[0] = 0 >= 0`).
- Orientation: this module emits a conventional TOP-DOWN geotransform
  (origin at the upper-left, negative dy) and reads bands with GDAL's
  default (also top-down) row order. Upstream pairs a bottom-up geotransform
  with `GDAL_NETCDF_BOTTOMUP=NO` - pairing a *default* top-down read with
  that bottom-up geotransform (an easy mistake - the bounding box still
  looks correct) silently flips the raster upside down. Avoiding the global
  config option also means it can't leak into unrelated GDAL use elsewhere
  in this app (e.g. fileextent.py).
- Staggered wind components (U, V, W) are destaggered here (averaged onto
  the mass grid) rather than skipped outright - cheap in numpy and makes
  wind fields viewable, which upstream's `# TODO support staggered vars`
  left undone.
"""
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import numpy as np
from osgeo import gdal

from gis4wrf.core import CRS, LonLat, UnsupportedError, UserError
from gis4wrf.core.constants import ProjectionTypes
from gis4wrf.core.readers.categories import LANDUSE

gdal.UseExceptions()

# Coordinate variables GDAL exposes as ordinary subdatasets but which are not
# themselves displayable fields.
_COORD_VAR_NAMES = {
    'XLAT', 'XLONG', 'XLAT_M', 'XLONG_M', 'XLAT_U', 'XLONG_U',
    'XLAT_V', 'XLONG_V', 'XLAT_C', 'XLONG_C', 'CLAT', 'CLONG', 'Times',
}

# WRF's fill value for missing data (in addition to whatever GDAL reports as
# each band's own NoData value).
_WRF_FILL_VALUE = 9.9692099683868690e+36

# WRF variables whose values are class indices, not physical quantities -
# LU_INDEX/IVGTYP index the file's own MMINLU landuse scheme (the same one
# _build_extra_dim looks up for land_cat's level labels below); the soil-type
# fields use a separate scheme WRF's LANDUSE.TBL (and so categories.py)
# doesn't carry, so they get generated colors/labels instead of named ones
# (see colormaps.categorical_lut's fallback path). Deliberately excludes
# LANDUSEF/SOILCTOP/GREENFRAC - those are *fractions per category level*,
# genuinely continuous, not category indices themselves.
_CATEGORICAL_VARS = {
    'LU_INDEX': 'landuse', 'IVGTYP': 'landuse',
    'ISLTYP': 'soil', 'SCT_DOM': 'soil', 'SCB_DOM': 'soil',
}

# Extra (non-spatial) dimensions this app knows how to label, mirroring
# gis4wrf.core's (unvendored) get_wrf_nc_extra_dims whitelist.
_EXTRA_DIM_LABELS = {
    'bottom_top': 'Vertical Level',
    'bottom_top_stag': 'Vertical Level',
    'num_metgrid_levels': 'Vertical Level',
    'soil_layers_stag': 'Soil Depth Layer',
    'land_cat': 'Land Use Category',
    'soil_cat': 'Soil Type Category',
    'month': 'Month',
}

@dataclass(frozen=True)
class WRFVariable:
    name: str
    description: str
    units: str
    extra_dim: Optional[str]  # dimension name, e.g. 'bottom_top'; None for 2D
    n_times: int
    n_levels: int  # 1 for 2D variables
    # None => a continuous physical quantity. Otherwise this variable's
    # values are category indices: the file's MMINLU landuse scheme name for
    # a known landuse-indexing variable, or '' when it's categorical but this
    # app has no named scheme for it (soil-type fields, or a units=='category'
    # variable outside _CATEGORICAL_VARS) - '' still renders correctly via
    # colormaps.categorical_lut's generated-color fallback.
    category_scheme: Optional[str] = None


@dataclass(frozen=True)
class ExtraDim:
    name: str
    label: str
    steps: List[str]


def _global_attr(gdal_metadata: dict, key: str) -> Optional[str]:
    """Global attributes appear as NC_GLOBAL#KEY normally, but as bare KEY
    when a .aux.xml PAM sidecar (or some other metadata domain) flattens
    them - seen on a real WRF file during development. Check both."""
    return gdal_metadata.get('NC_GLOBAL#' + key, gdal_metadata.get(key))


def _global_attr_float(gdal_metadata: dict, key: str) -> float:
    value = _global_attr(gdal_metadata, key)
    if value is None:
        raise UserError(f'Not a recognized WRF/WPS NetCDF file (missing global attribute {key!r}).')
    return float(value)


def _build_crs(gdal_metadata: dict) -> CRS:
    proj_id = int(_global_attr_float(gdal_metadata, 'MAP_PROJ'))

    if proj_id == ProjectionTypes.LAT_LON:
        pole_lat = _global_attr(gdal_metadata, 'POLE_LAT')
        pole_lon = _global_attr(gdal_metadata, 'POLE_LON')
        if pole_lat is not None and pole_lon is not None and \
                (float(pole_lat) != 90.0 or float(pole_lon) != 0.0):
            raise UnsupportedError('Geographic coordinate system with rotated pole is not supported')
        return CRS.create_lonlat()

    if proj_id == ProjectionTypes.LAMBERT_CONFORMAL:
        return CRS.create_lambert(
            truelat1=_global_attr_float(gdal_metadata, 'TRUELAT1'),
            truelat2=_global_attr_float(gdal_metadata, 'TRUELAT2'),
            origin=LonLat(
                lon=_global_attr_float(gdal_metadata, 'STAND_LON'),
                lat=_global_attr_float(gdal_metadata, 'MOAD_CEN_LAT')))

    if proj_id == ProjectionTypes.MERCATOR:
        return CRS.create_mercator(
            truelat1=_global_attr_float(gdal_metadata, 'TRUELAT1'),
            origin_lon=_global_attr_float(gdal_metadata, 'STAND_LON'))

    if proj_id == ProjectionTypes.POLAR_STEREOGRAPHIC:
        return CRS.create_polar(
            truelat1=_global_attr_float(gdal_metadata, 'TRUELAT1'),
            origin_lon=_global_attr_float(gdal_metadata, 'STAND_LON'))

    raise UnsupportedError(f'Projection {proj_id} is not supported')


def _read_corner_lonlat(path: str, var_name: str, row: int, col: int) -> Tuple[float, float]:
    """Reads a single (lon, lat) pair from a coordinate variable at
    (time 0, row, col), without materializing the whole array."""
    ds = gdal.Open(f'NETCDF:"{path}":{var_name}')
    band = ds.GetRasterBand(1)
    value = band.ReadAsArray(col, row, 1, 1)
    return float(value[0, 0])


def _build_geo_transform(path: str, crs: CRS, size: Tuple[int, int]) -> Tuple[float, float, float, float, float, float]:
    """Top-down geotransform (x_min, dx, 0, y_max, 0, -dy), derived from the
    staggered U/V coordinate grids (edge-based) rather than the mass grid
    (cell-centered - using it would offset the raster by half a cell).

    GDAL's classic API (used by _read_corner_lonlat) returns netCDF arrays
    top-down: row 0 is the NORTHERNMOST row, not the native file's row 0
    (which is southernmost - WRF's south_north dimension increases
    northward). Row indices below are chosen with that flip already
    accounted for, i.e. they name where each row actually sits on the
    ground, not the netCDF storage order.
    """
    nx, ny = size

    # XLONG_U/XLAT_U: shape (ny, nx + 1). GDAL row (ny - 1) is the southmost.
    south_row_u = ny - 1
    lon_sw_u = _read_corner_lonlat(path, 'XLONG_U', south_row_u, 0)
    lat_sw_u = _read_corner_lonlat(path, 'XLAT_U', south_row_u, 0)
    lon_se_u = _read_corner_lonlat(path, 'XLONG_U', south_row_u, nx)
    lat_se_u = _read_corner_lonlat(path, 'XLAT_U', south_row_u, nx)

    # XLONG_V/XLAT_V: shape (ny + 1, nx). GDAL row 0 is the northmost, row ny
    # the southmost.
    south_row_v = ny
    north_row_v = 0
    lon_sw_v = _read_corner_lonlat(path, 'XLONG_V', south_row_v, 0)
    lat_sw_v = _read_corner_lonlat(path, 'XLAT_V', south_row_v, 0)
    lon_nw_v = _read_corner_lonlat(path, 'XLONG_V', north_row_v, 0)
    lat_nw_v = _read_corner_lonlat(path, 'XLAT_V', north_row_v, 0)

    sw_u = crs.to_xy(LonLat(lon_sw_u, lat_sw_u))
    se_u = crs.to_xy(LonLat(lon_se_u, lat_se_u))
    sw_v = crs.to_xy(LonLat(lon_sw_v, lat_sw_v))
    nw_v = crs.to_xy(LonLat(lon_nw_v, lat_nw_v))

    dx = (se_u.x - sw_u.x) / nx
    dy = (nw_v.y - sw_v.y) / ny  # positive: north is a larger y than south

    x_min = sw_u.x
    y_max = nw_v.y
    return (x_min, dx, 0.0, y_max, 0.0, -dy)


def _open_netcdf(path: str) -> gdal.Dataset:
    """Opens `path` forcing GDAL's netCDF driver via the `NETCDF:"path"`
    syntax, rather than a bare `gdal.Open(path)`.

    WRF files built with netCDF4 (HDF5-backed) storage - the common case for
    real wrfout/wrfinput files, as opposed to the classic-format fixtures in
    tests/fixtures/ - are identified by GDAL's *HDF5* driver when opened
    bare, since that driver claims the file before the netCDF driver gets a
    look. That driver reports global attributes fine (so MAP_PROJ is still
    found) but lists subdatasets as `HDF5:"path"://VAR` instead of
    `NETCDF:"path":VAR`, and `NETCDF:"path":VAR` opens built from that name
    fail - which silently dropped every real variable down to a handful of
    unrelated ones that happened to still resolve. Forcing the netCDF driver
    up front avoids the ambiguity entirely and works identically on both
    classic and netCDF4/HDF5-backed files.
    """
    try:
        return gdal.Open(f'NETCDF:"{path}"')
    except RuntimeError:
        raise UserError(f'{path.rsplit("/", 1)[-1]} is not a NetCDF file.')


def _list_subdataset_names(path: str) -> List[str]:
    ds = _open_netcdf(path)
    subdatasets = ds.GetMetadata('SUBDATASETS')
    return sorted({v.rsplit(':', 1)[-1] for k, v in subdatasets.items() if k.endswith('_NAME')})


class WRFFile:
    """A single opened WRF/WPS NetCDF file: its CRS, geotransform, and the
    variables/time-steps/extra-dims available for display."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.name = path.rsplit('/', 1)[-1]

        root_ds = _open_netcdf(path)
        global_md = root_ds.GetMetadata()
        if _global_attr(global_md, 'MAP_PROJ') is None:
            raise UserError(f'{self.name} does not look like a WRF/WPS NetCDF file (no MAP_PROJ global attribute).')

        self.crs = _build_crs(global_md)

        var_names = _list_subdataset_names(path)
        if not var_names:
            raise UserError(f'{self.name} has no readable variables.')

        self.variables: Dict[str, WRFVariable] = {}
        self._stagger_axis_of: Dict[str, Optional[int]] = {}
        mminlu = _global_attr(global_md, 'MMINLU')

        for var_name in var_names:
            if var_name in _COORD_VAR_NAMES:
                continue
            try:
                var_ds = gdal.Open(f'NETCDF:"{path}":{var_name}')
            except RuntimeError:
                continue

            var_md = var_ds.GetMetadata()

            # MemoryOrder tells us directly whether this variable is
            # horizontally gridded ('XY ' or 'XYZ') as opposed to a 1D
            # vertical-only field like DZS/ZS/FCX/GCX ('Z  ', 'C  ', ...),
            # which GDAL still exposes as a (N, 1) "raster" - confirmed on a
            # real wrfout file, where these masqueraded as tiny rasters and,
            # before this check existed, corrupted mass-grid-size inference
            # (see _infer_mass_grid_size) enough to drop every real variable.
            memory_order = var_md.get(f'{var_name}#MemoryOrder', '').strip()
            if not memory_order.startswith('XY'):
                continue

            # NETCDF_DIM_EXTRA always includes 'Time' (even for a plain 2D
            # variable, confirmed on the geo_em fixture: '{Time}' with a
            # single band) - only a dimension *beyond* Time makes this a
            # variable with a selectable extra dimension (vertical level,
            # soil layer, ...).
            dim_extra = var_md.get('NETCDF_DIM_EXTRA', '').strip('{}')
            extra_dims_present = [d for d in dim_extra.split(',') if d and d != 'Time']
            extra_dim_name = extra_dims_present[0] if extra_dims_present else None

            n_levels = 1
            if extra_dim_name:
                # bands are ordered (extra_dim varies fastest within a time
                # step - verified: band k's NETCDF_DIM_<extra> increments
                # before NETCDF_DIM_Time does), so the level count is total
                # bands divided by the number of distinct time values.
                time_vals = set()
                for b in range(1, var_ds.RasterCount + 1):
                    band_md = var_ds.GetRasterBand(b).GetMetadata()
                    time_vals.add(band_md.get('NETCDF_DIM_Time'))
                n_times = len(time_vals) or 1
                n_levels = max(1, var_ds.RasterCount // n_times)
                if extra_dim_name not in _EXTRA_DIM_LABELS:
                    # Not a dimension we know how to label/select - skip it
                    # rather than show a variable no widget can control.
                    continue
            else:
                n_times = var_ds.RasterCount

            description = var_md.get(f'{var_name}#description', '') or ''
            units = (var_md.get(f'{var_name}#units', '') or '').strip()
            if units.strip('-').strip().lower() in ('', 'dimensionless', 'no units'):
                units = ''

            category_scheme: Optional[str]
            if var_name in _CATEGORICAL_VARS:
                scheme_kind = _CATEGORICAL_VARS[var_name]
                category_scheme = mminlu if scheme_kind == 'landuse' and mminlu else ''
            elif units.lower() == 'category':
                category_scheme = ''
            else:
                category_scheme = None

            self.variables[var_name] = WRFVariable(
                name=var_name, description=description.strip(), units=units,
                extra_dim=extra_dim_name, n_times=n_times, n_levels=n_levels,
                category_scheme=category_scheme)

        if not self.variables:
            raise UserError(f'{self.name} has no variables this app knows how to display.')

        # Determine the true mass-grid size, then resolve each variable's
        # stagger axis (if any) by comparing its raster shape against it -
        # more robust than guessing from variable name or dimension
        # metadata, and it naturally covers U/V/W plus any other staggered
        # field without a name-based special case.
        size = self._infer_mass_grid_size(path)
        self.size = size

        for var_name in list(self.variables):
            var_ds = gdal.Open(f'NETCDF:"{path}":{var_name}')
            nx_v, ny_v = var_ds.RasterXSize, var_ds.RasterYSize
            axis = None
            if nx_v == size[0] + 1 and ny_v == size[1]:
                axis = 1  # staggered along west_east
            elif ny_v == size[1] + 1 and nx_v == size[0]:
                axis = 0  # staggered along south_north
            elif nx_v != size[0] or ny_v != size[1]:
                # Doesn't fit the mass grid even after destaggering one axis -
                # not something this app can place correctly, drop it.
                del self.variables[var_name]
                continue
            self._stagger_axis_of[var_name] = axis

        self.geotransform = _build_geo_transform(path, self.crs, size)

        self.extra_dims: Dict[str, ExtraDim] = {}
        for var in self.variables.values():
            if var.extra_dim and var.extra_dim not in self.extra_dims:
                self.extra_dims[var.extra_dim] = self._build_extra_dim(var.extra_dim, var.n_levels, mminlu)

        # Time labels: the Times char variable cannot be read through either
        # GDAL API on the files this was validated against (classic API's
        # IReadBlock fails on the char dtype; the multidim API is blocked by
        # the unlimited Time dimension). Fall back to index-based labels;
        # revisit if a cheap real-timestamp read is found later.
        n_times = max((v.n_times for v in self.variables.values()), default=1)
        self.times = [f'Step {i + 1} of {n_times}' for i in range(n_times)]

    def _infer_mass_grid_size(self, path: str) -> Tuple[int, int]:
        candidates = []
        for var_name, var in self.variables.items():
            var_ds = gdal.Open(f'NETCDF:"{path}":{var_name}')
            candidates.append((var_ds.RasterXSize, var_ds.RasterYSize))
        # The mass-grid size is whichever (nx, ny) is smallest in both axes -
        # any staggered variable is exactly one cell larger along one axis.
        nx = min(c[0] for c in candidates)
        ny = min(c[1] for c in candidates)
        return nx, ny

    def _build_extra_dim(self, dim_name: str, n_levels: int, mminlu: Optional[str]) -> ExtraDim:
        label = _EXTRA_DIM_LABELS[dim_name]
        if dim_name == 'land_cat' and mminlu and mminlu in LANDUSE:
            categories = LANDUSE[mminlu]
            steps = [categories.get(i + 1, (f'Category {i + 1}', ''))[0] for i in range(n_levels)]
        else:
            steps = [f'{label} {i + 1}' for i in range(n_levels)]
        return ExtraDim(name=dim_name, label=label, steps=steps)

    def read(self, var_name: str, time_index: int, level_index: int) -> np.ndarray:
        """Returns a float32 (ny, nx) array on the mass grid (destaggered if
        necessary), top-down, with nodata/fill values converted to NaN."""
        if var_name not in self.variables:
            raise UserError(f'{var_name!r} is not available in {self.name}.')
        var = self.variables[var_name]

        var_ds = gdal.Open(f'NETCDF:"{self.path}":{var_name}')
        if var.extra_dim:
            band_index = time_index * var.n_levels + level_index + 1
        else:
            band_index = time_index + 1
        if not (1 <= band_index <= var_ds.RasterCount):
            raise UserError(
                f'{var_name} in {self.name} has no time/level index ({time_index}, {level_index}).')

        band = var_ds.GetRasterBand(band_index)
        data = band.ReadAsArray().astype(np.float32)

        nodata = band.GetNoDataValue()
        mask = np.isclose(data, _WRF_FILL_VALUE, rtol=1e-6)
        if nodata is not None:
            mask |= np.isclose(data, nodata, rtol=1e-6)
        data[mask] = np.nan

        axis = self._stagger_axis_of.get(var_name)
        if axis == 1:
            data = (data[:, :-1] + data[:, 1:]) / 2.0
        elif axis == 0:
            data = (data[:-1, :] + data[1:, :]) / 2.0

        return data
