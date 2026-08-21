# Plan: tree-structured (branching/sibling) nested domains

## Status

**Phase 1: done.** `gis4wrf.core` (`project.py`, `wps_namelist_to_project.py`,
`project_to_wps_namelist.py`) now stores domains WPS-native (root-first,
explicit `parent_id` per domain) and correctly imports/exports namelists
with sibling domains - verified with an exact round-trip of the real
`parent_id = 1,1,2,2` namelist (`tests/fixtures/namelist_siblings.wps`;
`tests/fixtures/namelist_hongkong.wps` is the linear-chain regression
fixture). `project_to_gdal_outlines.py` needed no changes (it only ever
iterated `project.bboxes` as a flat list).

`domainform.py` (the interactive UI) was **not** redesigned (that's still
Phase 2), but it did need two small compatibility adjustments since the
underlying storage order flipped from leaf-first to root-first:
- `populate_ui_from_project` now reads `map_proj`/`truelat1/2`/`stand_lon`
  from the actual root domain (`domains[0]`) rather than assuming
  `domains[0]` is the leaf being directly edited.
- It reduces the (potentially tree-shaped) domain list back to a single
  leaf-first chain for display, and raises a clear `UserError` - "Domain N
  has M nested domains sharing it as their parent... isn't supported by the
  current interface yet" - instead of crashing or silently displaying wrong
  data, if the loaded project actually has siblings (only reachable via
  namelist import right now, since the interactive form can only ever
  construct a linear chain itself).

Root's own `parent_cell_size_ratio`/padding fields no longer exist (it has
no parent, unlike the old schema where the outermost domain still carried
leftover derivation fields) - `populate_ui_from_project` defaults these to
sensible values (ratio 1, no padding) when displaying the outermost "parent
box" for a linear-chain project.

**Phase 2: done.** `domainform.py` was redesigned around a `QTreeWidget`
showing the actual domain tree (one item per domain, children nested under
their parent - so two domains sharing a parent render as two sibling tree
items, which is exactly the case Phase 1 unlocked). Selecting a domain shows
a properties panel that switches between "root" fields (Map Type,
Horizontal Resolution, Center Point - shown only for domain 1) and "nested"
fields (Nesting ratio, Position within Parent - `padding_left`/
`padding_bottom`, i.e. `i_parent_start`/`j_parent_start` minus one) for
everything else. "Add Child Domain" adds a domain under whichever tree item
is selected, so clicking a domain twice (once per child) produces two
siblings directly through the UI - no namelist round-trip needed to create
what Phase 1 could only *read*. "Remove Domain" removes the selected domain
and all its descendants (with a confirmation dialog if it has any),
renumbering the remaining domains' `parent_id` references to stay
WPS-valid. The old "linear chain of Parent N boxes + spinbox" UI and the
`padding_right`/`padding_top` input fields it exposed are gone - those were
specific to the old auto-derive-parent-size-from-padding convenience, which
doesn't generalize to a parent with multiple children (whose combined
extent would need a real union computation) and isn't needed now that
`domain_size` is a direct, per-domain field.

`domainoverlay.py`'s outline coloring changed from a fixed
red-for-main/blue-for-parent scheme (which relied on there being exactly
one "main" domain and a single linear chain) to a per-domain color drawn
from an 8-color palette, cycling by the domain's stable WPS number - so any
number of siblings at any depth stay visually distinguishable on the map,
not just "innermost vs. everything else".

Verified end-to-end through real Qt widget interaction (tree selection via
`setCurrentItem`, matching what an actual click does - not just calling
handlers directly): building a 3-domain project with two siblings entirely
through the UI from an empty project, confirming both children end up with
`parent_id` pointing at the same parent; importing the sibling namelist and
confirming the tree renders the correct hierarchy (root -> domain 2 ->
[domain 3, domain 4]) with each domain's panel showing its own distinct
ratio/position/size; an exact namelist export round-trip afterward;
partial removal of one sibling with correct renumbering of the survivor
(namelist re-export still byte-identical for the unaffected domain); the
existing linear-chain regression case; and a rendered screenshot showing 4
distinctly-colored, correctly-nested/positioned domain outlines over the
real geography, with two of them (siblings) visibly at different positions
within their shared parent. Also caught and fixed one real bug along the
way: removing every domain left `project.data['domains']` empty, which
`Project.projection` and the overlay redraw path hit as an unhandled
`IndexError` rather than the `UserError` every other "not configured yet"
case already raises - fixed in both places.

**Phase 3: done**, except for one part already covered elsewhere. Of item
11's two clauses: cascade-delete was already implemented as part of Phase
2's "Remove Domain" confirmation dialog (deleting a domain with descendants
asks for confirmation, then removes it and all descendants, renumbering
survivors). The remaining "per-branch ratio/resolution consistency checks"
clause is now enforced as a geometric containment check in
`Project.fill_domains()` (`project.py`): after computing each non-root
domain's bbox, it's compared against its parent's bbox (with a small
relative floating-point tolerance), and a `UserError` naming both domains
is raised if the child extends beyond the parent on any side. This is a
real WPS/geogrid.exe requirement (a child's placement + extent, scaled by
its nesting ratio, must stay within the parent's grid) that was previously
completely unchecked - a user could set padding/ratio/size fields (via the
UI, or a hand-edited/malformed namelist import) that placed a child outside
its parent, and the app would silently accept it and draw an incorrect
overlay. `domainform.py` needed no changes: every `fill_domains()` call
site already wraps it in `try/except UserError`, either surfacing the
message via `raise_on_invalid=True` (field edits) or silently skipping the
redraw (`draw_bbox_and_grids`), so the new check is caught by existing
error handling automatically.

Verified: both regression fixtures (`namelist_siblings.wps`,
`namelist_hongkong.wps`) still import and `fill_domains()` cleanly with
this check in place (no false positives from floating-point rounding); a
deliberately out-of-bounds domain (built both via direct `Project.data`
manipulation and via real Qt widget interaction - clicking "Add Root
Domain", "Add Child Domain", and setting ratio/padding/size fields through
the actual form widgets) correctly raises the new `UserError`; the same
child domain corrected to fit within its parent applies cleanly and
produces a bbox properly nested inside the parent's.

**Phase 4: in progress.** `pytest` added as a dev dependency
(`uv add --dev pytest`). Everything below runs with `uv run pytest`.

Done: `tests/test_core_domain_tree.py` - core-level (no Qt) coverage,
8 tests, all passing:
- `test_siblings_import_builds_correct_tree` - imports
  `namelist_siblings.wps`, asserts the exact parent_id tree (domains 3/4
  both children of domain 2), asserts the two siblings' bboxes don't
  overlap, and asserts every non-root domain fits inside its parent
  (Phase 3's check, re-verified at the data level).
- `test_siblings_export_round_trips` - exports that project back to a
  namelist (via `write_namelist` to a tmp file), re-imports it, and checks
  parent_id and bbox (tolerance 1.0, for float round-trip noise) match the
  original for every domain. This is plan item 12.
- `test_siblings_outlines_are_visually_distinguishable` - checks
  `convert_project_to_gdal_outlines` produces one feature per domain (4).
- `test_hongkong_linear_chain_still_works` / `test_hongkong_export_round_trips`
  - same shape of checks against the linear-chain regression fixture. Plan
  item 13.
- `test_set_domains_linear_chain_still_works` - exercises the old
  leaf-first convenience API (`Project.set_domains()`, still used by
  `DomainForm`'s "Add Child Domain" auto-derivation path historically, now
  superseded by direct dict construction but kept for API compatibility)
  under the new WPS-native storage, confirming it still produces a child
  properly nested inside its parent.
- `test_child_outside_parent_is_rejected` / `test_max_dom_mismatch_is_rejected`
  - re-assert the two `UserError` validation cases from Phase 1/3 as
  permanent regression tests (previously only checked via throwaway
  scripts during those phases).

Not yet done (pick up here):
- A Qt/UI-level test module (`tests/test_ui_domain_tree.py`, not yet
  created) exercising `DomainForm` through real widget interaction
  (`QT_QPA_PLATFORM=offscreen`), matching the rigor of the ad hoc scripts
  used to verify Phases 2 and 3 by hand, but persisted as an actual test:
  building a 3-domain project with two siblings entirely through the UI
  (click "Add Root Domain", select it, "Add Child Domain" twice) and
  confirming both children get the same `parent_id`; importing the sibling
  namelist through `on_import_from_namelist_button_clicked` and checking
  the resulting `QTreeWidget` shape (root -> domain 2 -> [domain 3,
  domain 4]); partial removal of one sibling via
  `on_remove_domain_button_clicked` and confirming correct renumbering;
  the out-of-bounds-child case surfacing as a caught `UserError` (not a
  crash) through `_apply_selected_domain_fields(raise_on_invalid=True)`
  when triggered via real field edits. Relevant `DomainForm` internals
  already located for this: `_rebuild_tree`/`on_tree_selection_changed`/
  `_selected_domain` (tree <-> `project.data['domains']` sync),
  `on_add_domain_button_clicked` (line ~363, first click with no domains
  yet adds a default root; subsequent clicks add a child under whichever
  tree item is selected), `on_remove_domain_button_clicked` (line ~395,
  cascade-collects descendants via a `children_of` map before removing,
  confirms via `QMessageBox` if more than one domain would be removed).
- No dedicated color-uniqueness assertion yet for
  `domainoverlay._pen_for_domain_number` (currently only the feature-count
  check above touches outline rendering at the core level) - worth adding
  once the UI test module exists, since that's where "visually
  distinguishable on the map" is actually meaningful to check end-to-end.


## Motivation

`gis4wrf.core.Project` only supports a linear nest chain (domain 0 ⊂ 1 ⊂ 2 ⊂
...), enforced at namelist-import time by rejecting anything else:

```
Due to the way domains are represented in GIS4WRF each parent domain can
have only one nested domain
```

Real WPS namelists don't have this restriction - `parent_id` is a plain
per-domain integer, and two domains can share a parent. Example that
triggers the rejection today (from a real namelist):

```
parent_id = 1, 1, 2, 2,
```

Domains 3 and 4 are both nested directly inside domain 2 - siblings, not a
chain. WPS itself has no problem with this; `gis4wrf.core`'s internal
representation is what's less general than the format it's supposed to
model.

## Root cause

`Project.data['domains']` is a flat list ordered innermost -> outermost,
where domain `i`'s parent is *implicitly* `domains[i+1]` - position in the
list **is** the parent relationship. This needs to become an explicit
`parent_id` per domain (matching WPS's own convention: the root domain's
`parent_id` points to itself), with the domain list in WPS order (root
first) instead of innermost-first.

## Phase 1 - Core data model (`gis4wrf.core`, `src/gis4wrf/core/` here)

1. Redefine the domain dict: add an explicit `parent_id` field instead of
   relying on list position. Keep the list WPS-ordered (root first).
2. Rewrite `Project.set_domains()` (`project.py`) / whatever replaces it to
   take a parent reference per domain instead of an ordered chain of
   "parent domain" kwargs.
3. Rewrite `Project.fill_domains()` (`project.py`): instead of walking the
   flat list bottom-up assuming `domains[idx-1]` is "the child," traverse
   top-down from the root, computing each domain's bbox from its actual
   parent (looked up by `parent_id`). WPS requires `parent_id < domain_id`,
   so a single forward pass over the WPS-ordered list is already correctly
   topologically sorted - this should end up *simpler* than the current
   code, not more complex.
4. Rewrite `wps_namelist_to_project.py`
   (`convert_nml_to_project_domains`): drop the
   `parent_id != [1] + list(range(1, max_dom))` rejection entirely; read
   `parent_id` directly per domain and build the tree. Simpler than
   today's forced-linearization loop.
5. Rewrite `project_to_wps_namelist.py`
   (`convert_project_to_wps_namelist`): emit `parent_id` straight from
   each domain's own field - no reconstruction from position needed.
6. Update `project_to_gdal_outlines.py` to iterate all domains by id
   rather than assuming an inner/outer position ordering. Projection
   metadata (`map_proj`, `truelat1/2`, `stand_lon`) stays on the root
   domain only, same as today - WPS defines one projection per project
   regardless of nesting shape.

## Phase 2 - UI (`domainform.py`) - the bigger redesign

7. Replace the current "linear chain of parent boxes + Number of Parent
   Domains spinbox" with an actual tree UI (`QTreeWidget` of domains; add/
   remove a child under a *selected* domain, so multiple children under
   one node are possible; per-domain ratio/padding/size fields shown for
   whichever domain is selected).
8. Rewrite `populate_ui_from_project` to recursively rebuild the tree
   widget from a loaded project instead of the old flat loop.
9. `domainoverlay.py`: still fine to keep root vs. non-root coloring, but
   sibling domains at the same level probably want distinguishable
   colors/labels so they're not visually ambiguous on the map.
10. "Set to Map View Extent" / "Set from File" / "Refresh View" currently
    implicitly operate on "domain 0" - with siblings there's no single
    unambiguous domain anymore, so these need to target whichever domain
    is selected in the tree widget.

## Phase 3 - Validation

11. Enforce: a domain can only be deleted if it has no children (or
    cascade-delete); per-branch ratio/resolution consistency checks, same
    idea as today but scoped per-parent instead of globally linear.

## Phase 4 - Testing

12. Primary round-trip test: the real namelist with `parent_id = 1,1,2,2`
    (see below) - import -> correct 4-domain tree with domains 3/4 as
    siblings under domain 2 -> export -> equivalent namelist -> outlines
    render with siblings visually distinguishable on the map.
13. Regression: existing linear-chain test files (the earlier Hong Kong
    3-domain namelist, the synthetic 2-domain one used during Phase 2 of
    the original port) still work under the new model - a linear chain is
    just a tree where every node has at most one child, so this should
    fall out for free if Phase 1 is done right.

## Suggested staging

Phase 1 alone is independently valuable and testable - correct import/
export/round-trip data even before the UI catches up (verifiable via
`gis4wrf.core` calls directly, same way prior work in this repo was
verified, before touching `domainform.py` at all). Phase 2 (the tree UI)
is the larger, separate effort and can reasonably follow as its own pass.

## Test fixture for Phase 1/4 (the motivating real-world case)

Saved as `tests/fixtures/namelist_siblings.wps` (with `max_dom` corrected
to 4, see note at the end of this section). `tests/fixtures/
namelist_hongkong.wps` is the linear-chain (non-sibling) regression
fixture used alongside it.

```
&share
 wrf_core = 'ARW',
 max_dom = 3
 start_date = '2019-06-01_00:00:00','2019-06-01_00:00:00','2019-06-01_00:00:00',
 end_date   = '2019-06-01_03:00:00','2019-06-01_03:00:00','2019-06-01_03:00:00',
 interval_seconds = 10800,
 io_form_geogrid = 2,
 opt_output_from_geogrid_path = '/nas/SWICE/D01D02/',
 debug_level = 0,
/

&geogrid
parent_id = 1, 1, 2, 2,
parent_grid_ratio = 1, 5, 5, 5,
i_parent_start = 1, 21,138,77
j_parent_start = 1, 21,135,77
e_we = 91, 251, 201, 201,
e_sn = 92, 256, 206, 206,
map_proj = 'lambert',
dx = 6250.0,
dy = 6250.0,
ref_lon = 6.621600000000001,
ref_lat = 46.518789854710775,
geog_data_res = 'default', 'default', 'ASTER+corine_modis+default','ASTER+corine_modis+default',
geog_data_path = '/data/WRF_DATA/WPS_GEOG/',
opt_geogrid_tbl_path= '/nas/SWICE/D01D02',
truelat1 = 46.4963,
truelat2 = 46.4963,
stand_lon = 6.6216,
/

&ungrib
 out_format = 'WPS',
 prefix = 'FILE',
/
```

Note: `max_dom = 3` in `&share` above doesn't match the 4 domains actually
defined in `&geogrid` (arrays of length 4) - that mismatch was in the
namelist as received. Fixed to `max_dom = 4` in the saved fixture file.
Separately, `wps_namelist_to_project.py` now raises a clear `UserError` if
this happens again on some other file ("max_dom (N) does not match the
number of domains actually defined (M)...") instead of silently
mis-parsing or crashing confusingly.
