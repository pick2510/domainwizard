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

**Phase 2 (tree UI), 3 (validation), 4 (broader testing): not started** -
see below, unchanged from the original plan.


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
