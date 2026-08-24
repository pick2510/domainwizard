"""Phase 0 of the View-tab plan: CRS.WRF_DATUM_PROJ4 must use the WRF sphere
(radius 6370000 m), not the WGS84 ellipsoid that upstream GIS4WRF used (per a
since-removed FIXME acknowledging it was wrong).

Why this matters here specifically: Project.fill_domains() builds a domain's
projection with +lat_0/+lon_0 set to that *same* domain's own center point
(see Project.projection), so converting the center itself back and forth
through CRS.to_xy()/to_lonlat() is a no-op regardless of datum - projecting a
point at a projection's own origin always returns (0, 0). The datum only
becomes observable away from the origin: at a bbox corner (a nonzero metre
offset from center), the spherical and WGS84-ellipsoid Lambert/Mercator/polar
formulas map that same metre offset to measurably different lon/lat, because
WRF itself computed grid spacing (DX/DY) using its own spherical formulas.
Using the sphere here reproduces where WRF actually places a domain; using
WGS84 does not, despite "looking" more standard.
"""
import os

import pytest

import gis4wrf.core as core
from gis4wrf.core.constants import WRF_PROJ4_SPHERE
from gis4wrf.core.readers.namelist import read_namelist
from gis4wrf.core.transforms.wps_namelist_to_project import convert_nml_to_project_domains

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
SIBLINGS_WPS = os.path.join(FIXTURES_DIR, 'namelist_siblings.wps')


def _root_bbox_corners_lonlat():
    nml = read_namelist(SIBLINGS_WPS, 'wps')
    project = core.Project.create()
    project.data['domains'] = convert_nml_to_project_domains(nml)
    project.fill_domains()
    root = project.data['domains'][0]
    proj = project.projection
    bottom_left = proj.to_lonlat(core.Coordinate2D(root['bbox'].minx, root['bbox'].miny))
    top_right = proj.to_lonlat(core.Coordinate2D(root['bbox'].maxx, root['bbox'].maxy))
    return bottom_left, top_right


def test_wrf_datum_is_the_wrf_sphere_not_wgs84():
    assert core.CRS.WRF_DATUM_PROJ4 == WRF_PROJ4_SPHERE


def test_root_domain_corner_lonlat_matches_sphere_datum():
    # Pinned once with the sphere datum in place (see module docstring for why
    # a bbox corner, not the center, is the point that actually exercises the
    # datum). If this regresses to something close to
    # (lon=3.122976, lat=43.904805) instead, WRF_DATUM_PROJ4 has silently
    # reverted to WGS84 - see test_datum_choice_measurably_shifts_corner_lonlat
    # for that comparison made explicit.
    bottom_left, top_right = _root_bbox_corners_lonlat()
    assert bottom_left.lon == pytest.approx(3.112809, abs=1e-5)
    assert bottom_left.lat == pytest.approx(43.905622, abs=1e-5)
    assert top_right.lon == pytest.approx(10.476400, abs=1e-5)
    assert top_right.lat == pytest.approx(49.014080, abs=1e-5)


def test_datum_choice_measurably_shifts_corner_lonlat():
    # Demonstrates *why* the datum matters (not just that it's set correctly):
    # swapping in the old WGS84 datum for this same fixture shifts a root
    # domain's bbox corner by roughly 1 km (~0.01 degrees longitude) on a
    # ~560 km-wide domain - well above floating-point noise, and exactly the
    # kind of silent misregistration the sphere datum fixes.
    import gis4wrf.core.crs as crsmod

    correct_bottom_left, _ = _root_bbox_corners_lonlat()

    original_datum = crsmod.CRS.WRF_DATUM_PROJ4
    try:
        crsmod.CRS.WRF_DATUM_PROJ4 = '+datum=WGS84'
        wgs84_bottom_left, _ = _root_bbox_corners_lonlat()
    finally:
        crsmod.CRS.WRF_DATUM_PROJ4 = original_datum

    lon_shift_deg = abs(wgs84_bottom_left.lon - correct_bottom_left.lon)
    assert lon_shift_deg > 0.005  # ~500 m+ at this latitude; noise would be ~1e-9
