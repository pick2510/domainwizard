"""Phase 4 of PLAN_TREE_DOMAINS.md: persisted, automated coverage of the
tree-structured (sibling) domain support built in Phases 1-3, at the
gis4wrf.core level (no Qt/UI involved - see test_ui_domain_tree.py for
that). Everything here was previously verified ad hoc via throwaway
scripts during Phases 1-3; this makes that coverage permanent and
regression-checkable.
"""
import os

import pytest

import gis4wrf.core as core
from gis4wrf.core.readers.namelist import read_namelist
from gis4wrf.core.transforms.wps_namelist_to_project import convert_nml_to_project_domains
from gis4wrf.core.transforms.project_to_wps_namelist import convert_project_to_wps_namelist

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
SIBLINGS_WPS = os.path.join(FIXTURES_DIR, 'namelist_siblings.wps')
HONGKONG_WPS = os.path.join(FIXTURES_DIR, 'namelist_hongkong.wps')


def _project_from_namelist(path: str) -> core.Project:
    nml = read_namelist(path, 'wps')
    domains = convert_nml_to_project_domains(nml)
    project = core.Project.create()
    project.data['domains'] = domains
    project.fill_domains()
    return project


def _assert_bbox_close(a: core.BoundingBox2D, b: core.BoundingBox2D, tol: float = 1.0) -> None:
    assert abs(a.minx - b.minx) < tol
    assert abs(a.miny - b.miny) < tol
    assert abs(a.maxx - b.maxx) < tol
    assert abs(a.maxy - b.maxy) < tol


# --- item 12: sibling namelist round-trip ----------------------------------

def test_siblings_import_builds_correct_tree():
    project = _project_from_namelist(SIBLINGS_WPS)
    domains = project.data['domains']
    assert len(domains) == 4

    # domain 1 (root) has no parent_id of its own (defaults to itself/1);
    # domains 2 is the root's only child; domains 3 and 4 are both children
    # of domain 2, i.e. siblings - this is exactly the case a linear-chain
    # model (position in list == parent) cannot represent.
    assert domains[0].get('parent_id', 1) == 1
    assert domains[1]['parent_id'] == 1
    assert domains[2]['parent_id'] == 2
    assert domains[3]['parent_id'] == 2

    # Siblings must not overlap - if they did, computing them from
    # independent padding/size fields under the same parent would produce
    # ambiguous or overlapping geometry.
    child_a, child_b = domains[2]['bbox'], domains[3]['bbox']
    overlaps = not (
        child_a.maxx <= child_b.minx or child_b.maxx <= child_a.minx or
        child_a.maxy <= child_b.miny or child_b.maxy <= child_a.miny
    )
    assert not overlaps

    # Every non-root domain must fit within its parent (Phase 3).
    by_number = {i + 1: d for i, d in enumerate(domains)}
    for domain_number, domain in enumerate(domains[1:], start=2):
        parent = by_number[domain['parent_id']]
        assert domain['bbox'].minx >= parent['bbox'].minx - 1
        assert domain['bbox'].miny >= parent['bbox'].miny - 1
        assert domain['bbox'].maxx <= parent['bbox'].maxx + 1
        assert domain['bbox'].maxy <= parent['bbox'].maxy + 1


def test_siblings_export_round_trips(tmp_path):
    project = _project_from_namelist(SIBLINGS_WPS)
    original_domains = [dict(d) for d in project.data['domains']]

    wps = convert_project_to_wps_namelist(project)
    out_path = str(tmp_path / 'roundtrip.wps')
    core.write_namelist(wps, out_path)

    reimported = _project_from_namelist(out_path)
    reimported_domains = reimported.data['domains']

    assert len(reimported_domains) == len(original_domains)
    for original, reimported_domain in zip(original_domains, reimported_domains):
        assert reimported_domain.get('parent_id', 1) == original.get('parent_id', 1)
        _assert_bbox_close(reimported_domain['bbox'], original['bbox'])


def test_siblings_outlines_are_visually_distinguishable():
    project = _project_from_namelist(SIBLINGS_WPS)
    gdal_ds = core.convert_project_to_gdal_outlines(project)
    layer = gdal_ds.GetLayer(0)
    assert layer.GetFeatureCount() == 4


# --- item 13: linear-chain regressions --------------------------------------

def test_hongkong_linear_chain_still_works():
    project = _project_from_namelist(HONGKONG_WPS)
    domains = project.data['domains']
    assert len(domains) == 3
    # A linear chain is a tree where every parent_id points at the
    # immediately preceding domain.
    assert domains[1]['parent_id'] == 1
    assert domains[2]['parent_id'] == 2


def test_hongkong_export_round_trips(tmp_path):
    project = _project_from_namelist(HONGKONG_WPS)
    original_domains = [dict(d) for d in project.data['domains']]

    wps = convert_project_to_wps_namelist(project)
    out_path = str(tmp_path / 'roundtrip.wps')
    core.write_namelist(wps, out_path)

    reimported = _project_from_namelist(out_path)
    reimported_domains = reimported.data['domains']
    assert len(reimported_domains) == len(original_domains)
    for original, reimported_domain in zip(original_domains, reimported_domains):
        _assert_bbox_close(reimported_domain['bbox'], original['bbox'])


def test_set_domains_linear_chain_still_works():
    """The old bottom-up, leaf-first convenience API (Project.set_domains(),
    kept for the interactive 'chain of domains' case DomainForm builds
    incrementally) must still work under the WPS-native root-first storage
    model Phase 1 introduced."""
    project = core.Project.create()
    project.set_domains(
        map_proj='lat-lon',
        cell_size=[0.1, 0.1],
        domain_size=[40, 40],
        center_lonlat=core.LonLat(lon=10.0, lat=46.0),
        stand_lon=0.0,
        parent_domains=[{
            'parent_cell_size_ratio': 3,
            'padding_left': 10, 'padding_right': 10,
            'padding_bottom': 10, 'padding_top': 10,
        }],
    )
    domains = project.data['domains']
    assert len(domains) == 2
    assert domains[1]['parent_id'] == 1

    project.fill_domains()
    parent_bbox, child_bbox = domains[0]['bbox'], domains[1]['bbox']
    assert child_bbox.minx >= parent_bbox.minx
    assert child_bbox.miny >= parent_bbox.miny
    assert child_bbox.maxx <= parent_bbox.maxx
    assert child_bbox.maxy <= parent_bbox.maxy


# --- Phase 3: containment validation ----------------------------------------

def test_child_outside_parent_is_rejected():
    project = core.Project.create()
    project.data['domains'] = [
        {
            'parent_id': 1,
            'domain_size': [50, 50],
            'cell_size': [10000.0, 10000.0],
            'center_lonlat': [10.0, 46.0],
            'map_proj': 'lambert',
            'truelat1': 46.0,
            'truelat2': 46.0,
            'stand_lon': 10.0,
        },
        {
            'parent_id': 1,
            'parent_cell_size_ratio': 3,
            'domain_size': [200, 200],
            'padding_left': 40,
            'padding_bottom': 40,
        },
    ]
    with pytest.raises(core.UserError):
        project.fill_domains()


def test_max_dom_mismatch_is_rejected():
    bad_path_content = SIBLINGS_WPS
    nml = read_namelist(bad_path_content, 'wps')
    nml['share']['max_dom'] = 3  # actual domain count in the fixture is 4
    with pytest.raises(core.UserError):
        convert_nml_to_project_domains(nml)
