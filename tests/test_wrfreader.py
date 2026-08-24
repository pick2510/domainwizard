"""Phase 1 of the View-tab plan: gis4wrf-free WRF/WPS NetCDF access
(wrfreader.py), tested against real (spatially/variable-trimmed) fixture
files rather than synthetic ones, so the CRS/geotransform math is checked
against genuine WRF-computed coordinate arrays.

tests/fixtures/geo_em_small.nc: an 8x8 window of a real geo_em file
(Mercator projection), static fields (HGT_M, LU_INDEX), single time step.
tests/fixtures/wrfout_multitime.nc: the same 8x8 window of a real wrfout
file, 3 (synthetic, via ncap2/ncrcat) time steps with distinct T2/XTIME
values, T2 (2D) and U/V (3-level, staggered) variables.
tests/fixtures/wrfout_with_1d_var.nc: an 8x8 window of a real wrfout file
that also carries ZS/DZS (1D, vertical-only soil-layer variables, MemoryOrder
'Z  ') alongside T2/HGT (2D) - regression coverage for a real bug where GDAL
exposes 1D variables as tiny (N, 1) "rasters" that corrupted mass-grid-size
inference and silently dropped every real 2D variable from a genuine wrfout
file (see wrfreader.py's MemoryOrder check).
"""
import os

import numpy as np
import pytest
from osgeo import gdal

from gis4wrf.core.errors import UnsupportedError, UserError
from domainwizard.wrfreader import WRFFile, _build_crs

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
GEO_EM = os.path.join(FIXTURES_DIR, 'geo_em_small.nc')
WRFOUT = os.path.join(FIXTURES_DIR, 'wrfout_multitime.nc')
WRFOUT_WITH_1D_VAR = os.path.join(FIXTURES_DIR, 'wrfout_with_1d_var.nc')


def _declared_dx_dy(path):
    ds = gdal.Open(path)
    g = ds.GetMetadata()
    dx = float(g.get('NC_GLOBAL#DX', g.get('DX')))
    dy = float(g.get('NC_GLOBAL#DY', g.get('DY')))
    return dx, dy


# --- geotransform / CRS accuracy --------------------------------------------

@pytest.mark.parametrize('path', [GEO_EM, WRFOUT])
def test_geotransform_matches_declared_dx_dy(path):
    f = WRFFile(path)
    declared_dx, declared_dy = _declared_dx_dy(path)
    dx = f.geotransform[1]
    dy = -f.geotransform[5]  # top-down: dy is stored negative

    assert dx == pytest.approx(declared_dx, rel=1e-3)
    assert dy == pytest.approx(declared_dy, rel=1e-3)


@pytest.mark.parametrize('path', [GEO_EM, WRFOUT])
def test_geotransform_is_top_down(path):
    """Row 0 of WRFFile.read() must be the northernmost row, matching a
    geotransform with y_max at the origin and negative dy - the orientation
    bug this module was specifically written to avoid (see its docstring)."""
    f = WRFFile(path)
    lat_var = 'XLAT_M' if 'XLAT_M' in _list(path) else 'XLAT'
    var_ds = gdal.Open(f'NETCDF:"{path}":{lat_var}')
    lat = var_ds.GetRasterBand(1).ReadAsArray()
    assert lat[0].mean() > lat[-1].mean()  # row 0 is further north
    assert f.geotransform[5] < 0  # negative dy pairs with a top-down read


def _list(path):
    ds = gdal.Open(path)
    md = ds.GetMetadata('SUBDATASETS')
    return {v.rsplit(':', 1)[-1] for k, v in md.items() if k.endswith('_NAME')}


# --- CRS construction --------------------------------------------------------

def test_lambert_and_mercator_and_polar_build_a_crs():
    lambert = _build_crs({
        'NC_GLOBAL#MAP_PROJ': '1', 'NC_GLOBAL#TRUELAT1': '30', 'NC_GLOBAL#TRUELAT2': '60',
        'NC_GLOBAL#STAND_LON': '-95', 'NC_GLOBAL#MOAD_CEN_LAT': '40',
    })
    assert '+proj=lcc' in lambert.proj4

    mercator = _build_crs({
        'NC_GLOBAL#MAP_PROJ': '3', 'NC_GLOBAL#TRUELAT1': '22.3', 'NC_GLOBAL#STAND_LON': '114.176',
    })
    assert '+proj=merc' in mercator.proj4

    polar = _build_crs({
        'NC_GLOBAL#MAP_PROJ': '2', 'NC_GLOBAL#TRUELAT1': '60', 'NC_GLOBAL#STAND_LON': '-100',
    })
    assert '+proj=stere' in polar.proj4

    lonlat = _build_crs({'NC_GLOBAL#MAP_PROJ': '6'})
    assert '+proj=latlong' in lonlat.proj4


def test_rotated_pole_lat_lon_is_rejected():
    with pytest.raises(UnsupportedError):
        _build_crs({
            'NC_GLOBAL#MAP_PROJ': '6', 'NC_GLOBAL#POLE_LAT': '80', 'NC_GLOBAL#POLE_LON': '0',
        })


def test_unknown_projection_is_rejected():
    with pytest.raises(UnsupportedError):
        _build_crs({'NC_GLOBAL#MAP_PROJ': '99'})


def test_not_a_wrf_file_is_rejected(tmp_path):
    # any GDAL-openable file lacking a MAP_PROJ global attribute
    bogus = str(tmp_path / 'bogus.tif')
    gdal.GetDriverByName('GTiff').Create(bogus, 4, 4, 1)
    with pytest.raises(UserError):
        WRFFile(bogus)


# --- variable / time / level enumeration ------------------------------------

def test_geo_em_exposes_static_2d_variables_with_no_extra_dim():
    f = WRFFile(GEO_EM)
    assert set(f.variables) == {'HGT_M', 'LU_INDEX'}
    for var in f.variables.values():
        assert var.extra_dim is None
        assert var.n_times == 1
        assert var.n_levels == 1
    assert f.extra_dims == {}


def test_1d_vertical_only_variables_are_excluded_and_dont_break_2d_ones():
    f = WRFFile(WRFOUT_WITH_1D_VAR)
    assert 'ZS' not in f.variables
    assert 'DZS' not in f.variables
    assert {'T2', 'HGT'} <= set(f.variables)
    assert f.size == (8, 8)


def test_wrfout_exposes_2d_and_3d_variables_with_correct_time_and_level_counts():
    f = WRFFile(WRFOUT)
    assert f.variables['T2'].extra_dim is None
    assert f.variables['T2'].n_times == 3
    assert f.variables['T2'].n_levels == 1

    assert f.variables['U'].extra_dim == 'bottom_top'
    assert f.variables['U'].n_times == 3
    assert f.variables['U'].n_levels == 3
    assert 'bottom_top' in f.extra_dims
    assert f.extra_dims['bottom_top'].label == 'Vertical Level'
    assert len(f.extra_dims['bottom_top'].steps) == 3

    assert len(f.times) == 3


def test_time_index_selects_distinct_data():
    f = WRFFile(WRFOUT)
    means = [float(np.nanmean(f.read('T2', t, 0))) for t in range(3)]
    assert means[0] < means[1] < means[2]  # fixture was built with +1K per step


def test_level_index_selects_distinct_data():
    f = WRFFile(WRFOUT)
    means = [float(np.nanmean(f.read('U', 0, lvl))) for lvl in range(3)]
    assert len(set(round(m, 3) for m in means)) == 3


def test_out_of_range_time_or_level_is_rejected():
    f = WRFFile(WRFOUT)
    with pytest.raises(UserError):
        f.read('T2', 99, 0)
    with pytest.raises(UserError):
        f.read('U', 0, 99)


def test_unknown_variable_is_rejected():
    f = WRFFile(GEO_EM)
    with pytest.raises(UserError):
        f.read('NOT_A_REAL_VARIABLE', 0, 0)


# --- destaggering -------------------------------------------------------------

def test_staggered_wind_components_are_destaggered_to_the_mass_grid():
    f = WRFFile(WRFOUT)
    nx, ny = f.size
    u = f.read('U', 0, 0)
    v = f.read('V', 0, 0)
    assert u.shape == (ny, nx)
    assert v.shape == (ny, nx)


def test_destagger_is_the_average_of_adjacent_staggered_cells():
    f = WRFFile(WRFOUT)
    raw_ds = gdal.Open(f'NETCDF:"{WRFOUT}":U')  # keep alive: band holds a weak ref
    raw = raw_ds.GetRasterBand(1).ReadAsArray().astype('float64')
    destaggered = f.read('U', 0, 0)
    expected = (raw[:, :-1] + raw[:, 1:]) / 2.0
    np.testing.assert_allclose(destaggered, expected, rtol=1e-5)
