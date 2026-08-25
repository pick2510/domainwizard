# Native C++ port status

The native implementation lives in `cpp/` on the `cplusplus` branch. It is a
parallel C++20 / Qt 6 / GDAL application; the Python implementation remains
the feature-complete behavioral reference.

## Completed

- CMake build, CTest integration, Catch2 tests (per-case discovery for the
  headless core suite), Linux install target, and macOS Homebrew build
  instructions / CI job. UI code lives in its own `wrftools_ui` library so
  `main_window`/`view_form`/`domain_form`/`tile_map_widget` are linkable from
  tests, not just the executable.
- Qt main window with shared map and native Domains / View tabs.
- OpenStreetMap XYZ map widget: Web Mercator pan and zoom, asynchronous tile
  requests, memory/disk cache with temporary-directory fallback, **named,
  z-ordered raster/vector overlay groups** (so the Domains tab's outlines and
  the View tab's raster layers redraw independently instead of clobbering
  each other), movable legend, movable info-text overlay, and map-image
  export.
- **Geospatial core (`crs.hpp`/`crs.cpp`, `warp.hpp`/`warp.cpp`)**: WRF
  sphere CRS built from the same proj4 strings as `gis4wrf.core.crs` (fixes a
  real bug - the previous `OGRSpatialReference::SetPS`/`SetMercator` calls
  passed `truelat1` as a scale-1 origin latitude instead of the standard
  parallel, mislocating every non-Lambert domain); geotransform derived from
  the staggered `XLONG_U/XLAT_U`/`XLONG_V/XLAT_V` edge grids, not mass-grid
  corners; GDAL MEM + `GDALWarp` reprojection to EPSG:3857 for raster
  placement (replacing a bare stretch into a lon/lat box, which was only
  correct for near-equatorial lat/lon domains). All three are pinned against
  live Python-produced values in `core_tests.cpp`.
- WPS domain model: root-first explicit parent tree, sibling validation,
  subtree deletion and WPS ID renumbering, **plus real geometry** -
  `DomainProject::projection()`/`fillDomains()` port `Project.projection`/
  `fill_domains()` (bbox per domain, child-relative-tolerance containment
  check, center lon/lat), pinned against `gis4wrf.core.Project.bboxes` for
  the sibling fixture.
- WPS namelist import/export for the currently supported domain subset
  (including sibling and linear-chain fixtures), now also carrying the
  root's projection/reference-point fields on the domain itself rather than
  a separate project-level struct. The parser handles an array value wrapped
  across multiple lines; the writer's output shape matches
  `project_to_wps_namelist`'s exactly (`nocolons`, the synthetic `&metgrid`
  group) - including that neither reference preserves an imported
  namelist's other fields (`start_date`/`geog_data_*`/`&ungrib`/
  `&mod_levs`, ...): `convert_project_to_wps_namelist` reconstructs from
  `Project` fields too, verified by diffing a real round-trip, so that data
  loss was never a C++-only gap (an earlier version of this doc claimed
  otherwise).
- Domains UI: import/export, tree display (domain id carried in
  `Qt::UserRole`, not parsed from display text), add child, cascade remove,
  **editable root panel** (projection, truelat1/2, stand_lon, resolution,
  center point) and **nested panel** (ratio, position within parent) with
  green/yellow/red field validation styling, **set-from-map-extent and
  set-from-file-extent actions** (`file_extent.hpp`/`.cpp`), and **domain
  outlines drawn on the shared map** (8-color palette cycled by stable WPS
  domain number, densified via `perimeter/200` segmentization so curved
  projections render correctly, zoom-to-domain over the densified outline).
  A `setActive` gate keeps field edits from yanking the camera while the
  View tab is on screen.
- WRF reader: GDAL NetCDF variable discovery, non-spatial-field filtering,
  time/level selection, nodata conversion, U/V-style destaggering, geographic
  bounds (now derived from the corrected projected geotransform, not raw
  corner samples).
- WRF filename parsing and lazy multi-file series reads.
- Rendering primitives: explicit unit conversions, continuous colormaps,
  **real WRF LANDUSE categorical palettes** (USGS 31 entries,
  MODIFIED_IGBP_MODIS_NOAH 24 entries, vendored from `LANDUSE.TBL` via
  `gis4wrf.core.readers.categories`, with a deterministic 8-color fallback +
  generated label for unknown schemes/values - e.g. soil-type fields, which
  have no table), RGBA output, Qt image conversion, colorbar pixmap with
  `auto`/`fixed`/`scientific` tick formatting, and a **categorical swatch
  legend** (20-row cap + "+N more").
- **View UI (`wrf_source.hpp`/`.cpp`, rewritten `view_form.hpp`/`.cpp`)**: a
  real multi-layer stack - `WrfSource`/`WrfSourceRegistry` let `WrfFile` and
  `WrfFileSeries` be opened and rendered interchangeably (mirroring
  `wrfseries.py`'s "mirrors WRFFile's surface exactly" design), keyed by
  path so a file already open (singly or as part of a series) is reused, not
  reopened. Files tree (multi-select open, close-with-layers-in-use
  confirmation) and Layers tree (checkable visibility, add/remove, move up/
  down, topmost-first display over a bottom-first draw-order list) are
  separate, independently populated widgets. Each layer carries its own
  variable/time/level/colormap/units/opacity/range/interpolate; a
  `WrfFileSeries` shows one file row with its real `%Y-%m-%d %H:%M`
  timestamp labels instead of "Step i of N", and a lazily-detected series
  mismatch drops just that layer's overlay (caught around each render call
  in `refreshMap`) instead of failing the whole redraw. Zoom-to-layer,
  categorical/continuous colorbar and movable info-text overlay, and
  looping playback are wired per the selected layer.
- **`LayerRenderer` (`layer_renderer.hpp`/`.cpp`)**: the two-tier cache the
  View UI above actually renders through - a byte-bounded cache of warped
  (EPSG:3857, native-unit) slices keyed by (file, variable, time, level),
  and a count-bounded cache of colormapped images keyed by the slice key
  plus (colormap, vmin, vmax, unit), so a UI change that only touches
  opacity/visibility/colormap/range doesn't re-read and re-warp the GDAL
  subdataset. `renderLayer`/`colorizeWarped` (`raster_layer.hpp`) stay as
  the uncached path the cache itself and direct tests build on. Mirrors
  `wrftools.rasterlayer.LayerRenderer`'s cache tiers and byte/count budgets.
- Native regression suite, now including cross-language pins: CRS/geotransform,
  warped-raster bounds, domain bboxes, and LANDUSE palette values are each
  checked against a value produced by running the Python reference on the
  same fixture, not just internal self-consistency. Real Qt widget-interaction
  tests for `DomainForm` (tree shape from a real namelist, selection via
  `setCurrentItem`) and `ViewForm` (a 4-file series collapsing to one file
  row, a 2-layer stack built and hidden via real checkbox clicks) in a new
  `wrftools_ui_tests` binary. Current local result: all CTest targets pass
  (25 CTest entries; `wrftools_ui_tests` alone runs 6 Catch2 cases).
- `RasterLayer` carries per-layer `tickCount`/`tickFormat`/`tickDecimals`,
  wired to a "Colorbar" group in `ViewForm` (tick format `auto`/`fixed`/
  `scientific`, decimals only enabled for the latter two). Deliberately
  excluded from `LayerRenderer`'s cache keys and routed straight to
  `updateColorbar()` rather than `refreshMap()` - a cosmetic legend tweak
  must not re-render or touch the slice/image cache, matching
  `wrftools.viewform`'s tick handlers.

## Still required for feature parity

### Packaging and verification

- Bundle Qt, GDAL, PROJ, GDAL data, and `proj.db` into a macOS `.app` and a
  portable Linux artifact; test both artifacts on clean machines.
- Port the remaining Python tests (197 total in the Python suite; the native
  suite has grown from 17 to 25 CTest entries / ~28 Catch2 cases but is
  still far short) - especially `tests/test_ui_view_layers.py`'s 43 cases,
  now that the layer stack exists to test against.
- Run and require the macOS CI job, then address any Homebrew-specific
  compiler or deployment findings.

## Non-goals retained from Python

- No WPS binary geographical datasets.
- No checked support for antimeridian-crossing or polar display domains.
- No rotated lat/lon WRF/WPS projection support.
- Single-file internal times remain index-labelled when GDAL cannot expose
  `Times`; filename-based multi-file series can expose real labels.
