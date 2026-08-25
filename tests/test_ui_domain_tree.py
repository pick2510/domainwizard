"""Phase 4 of PLAN_TREE_DOMAINS.md: Qt/UI-level coverage of the tree-structured
(sibling) domain support, exercising DomainForm through real widget
interaction (QTreeWidget selection via setCurrentItem, matching what an
actual click does) rather than calling its handlers directly with hand-built
state. See test_core_domain_tree.py for the equivalent gis4wrf.core-only
coverage (no Qt involved).

Requires QT_QPA_PLATFORM=offscreen (or a real display) - set via os.environ
below if not already configured, so `uv run pytest` works out of the box.
"""
import os

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import pytest
from PyQt6.QtWidgets import QApplication, QFileDialog

import gis4wrf.core as core
from wrftools.domainform import DomainForm, DOMAIN_NUMBER_ROLE
from wrftools.tilemap import TileMapWidget

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
SIBLINGS_WPS = os.path.join(FIXTURES_DIR, 'namelist_siblings.wps')


@pytest.fixture(scope='session')
def qapp():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def form(qapp):
    map_widget = TileMapWidget('https://example.invalid/{z}/{x}/{y}.png')
    return DomainForm(map_widget)


def _select(form, item):
    form.domain_tree.setCurrentItem(item)


def _root_item(form):
    return form.domain_tree.topLevelItem(0)


# --- building siblings entirely through the UI -------------------------------

def test_build_siblings_through_ui(form):
    form.on_add_domain_button_clicked()  # first click with no domains: adds the root
    assert form.domain_tree.topLevelItemCount() == 1

    _select(form, _root_item(form))
    form.on_add_domain_button_clicked()  # first child

    _select(form, _root_item(form))
    form.on_add_domain_button_clicked()  # second child of the same parent: a sibling

    domains = form.project.data['domains']
    assert len(domains) == 3
    assert domains[1]['parent_id'] == 1
    assert domains[2]['parent_id'] == 1

    # The tree widget itself (not just project.data) reflects the sibling
    # shape: two children under the same parent item.
    assert _root_item(form).childCount() == 2


# --- importing a namelist with siblings --------------------------------------

def test_import_namelist_builds_sibling_tree(form, monkeypatch):
    monkeypatch.setattr(QFileDialog, 'getOpenFileName', staticmethod(lambda *a, **k: (SIBLINGS_WPS, '')))
    form.on_import_from_namelist_button_clicked()

    domains = form.project.data['domains']
    assert len(domains) == 4
    assert domains[1]['parent_id'] == 1
    assert domains[2]['parent_id'] == 2
    assert domains[3]['parent_id'] == 2

    root_item = _root_item(form)
    assert root_item.childCount() == 1
    domain2_item = root_item.child(0)
    assert domain2_item.data(0, DOMAIN_NUMBER_ROLE) == 2
    assert domain2_item.childCount() == 2
    sibling_numbers = sorted(domain2_item.child(i).data(0, DOMAIN_NUMBER_ROLE) for i in range(2))
    assert sibling_numbers == [3, 4]


# --- removing one sibling renumbers the survivor -----------------------------

def test_remove_one_sibling_renumbers_survivor(form, monkeypatch):
    monkeypatch.setattr(QFileDialog, 'getOpenFileName', staticmethod(lambda *a, **k: (SIBLINGS_WPS, '')))
    form.on_import_from_namelist_button_clicked()

    domain2_item = _root_item(form).child(0)
    domain3_item = domain2_item.child(0)
    assert domain3_item.data(0, DOMAIN_NUMBER_ROLE) == 3
    survivor_bbox_before = form.project.data['domains'][3]['bbox']  # domain 4, untouched

    # Domain 3 is a leaf (no children of its own), so removing it doesn't
    # trigger the cascade-delete confirmation dialog.
    _select(form, domain3_item)
    form.on_remove_domain_button_clicked()

    domains = form.project.data['domains']
    assert len(domains) == 3
    # Old domain 4 is renumbered to 3, still parented under (unrenumbered) domain 2.
    assert domains[2]['parent_id'] == 2
    assert domains[2]['bbox'].minx == pytest.approx(survivor_bbox_before.minx)
    assert domains[2]['bbox'].maxy == pytest.approx(survivor_bbox_before.maxy)


# --- an out-of-bounds child surfaces as a caught UserError, not a crash ------

def test_out_of_bounds_child_raises_usererror_via_field_edit(form):
    form.on_add_domain_button_clicked()  # root
    _select(form, _root_item(form))
    form.on_add_domain_button_clicked()  # child, default ratio/padding/size fits within the root

    child_item = _root_item(form).child(0)
    _select(form, child_item)
    assert form._apply_selected_domain_fields(raise_on_invalid=True)

    # Push the child's position far outside the parent's extent through the
    # actual "Position within Parent" fields, same as a user typing into them.
    form.padding_left.set_value(1000)
    form.padding_bottom.set_value(1000)
    with pytest.raises(core.UserError):
        form._apply_selected_domain_fields(raise_on_invalid=True)
