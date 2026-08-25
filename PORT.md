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
  a separate project-level struct.
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
- View UI: open/close a file, variable/time/level/units/colormap controls,
  preview, map overlay (now placed via the warp pipeline above), auto-zoom,
  colorbar (continuous and categorical), visibility, and looping playback.
- Native regression suite, now including cross-language pins: CRS/geotransform,
  warped-raster bounds, domain bboxes, and LANDUSE palette values are each
  checked against a value produced by running the Python reference on the
  same fixture, not just internal self-consistency. Real Qt widget-interaction
  tests for `DomainForm` (tree shape from a real namelist, selection via
  `setCurrentItem`) in a new `wrftools_ui_tests` binary. Current local
  result: all CTest targets pass (23 cases).

## Still required for feature parity

### View tab and rendering

- Add a true multi-layer stack: Files/Layers tree widgets, add/remove/
  reorder, independently stored per-layer settings, simultaneous map
  overlays (the overlay-group plumbing this needs already exists in
  `TileMapWidget`; `ViewForm` itself still holds one `WrfFile` and renders
  one layer).
- Add byte-bounded warped-slice and count-bounded rendered-image caches
  (`WrfFile::read`/`renderLayer` still reopen and re-warp on every call).
- Connect `WrfFileSeries` to the View UI (it exists and is tested at the
  core level, but nothing in `view_form.cpp` opens one), including real
  `%Y-%m-%d %H:%M` timestamp labels and lazy mismatch errors degrading to
  "no overlay" instead of propagating.
- Add zoom-to-selected-layer, interpolation control, confirmation dialogs on
  close-file-in-use, and the remaining close/remove edge cases from the
  Python UI.
- Tick-count/format/decimals controls and the movable info overlay exist at
  the `TileMapWidget`/`colorbar` level but aren't wired into `ViewForm`'s UI
  yet.

### Domains tab

- Namelist import is still line-oriented and will silently truncate a
  multi-line array continuation (uncommon but valid WPS syntax); export is
  still lossy - it drops every namelist group/key outside the `geogrid`
  domain subset (`start_date`, `geog_data_*`, `&metgrid`, `&ungrib`,
  `&mod_levs`, ...), so round-tripping a real namelist through the app loses
  data.

### Packaging and verification

- Bundle Qt, GDAL, PROJ, GDAL data, and `proj.db` into a macOS `.app` and a
  portable Linux artifact; test both artifacts on clean machines.
- Port the remaining Python tests (197 total in the Python suite; the native
  suite has grown from 17 to 23 cases but is still far short) - especially
  `tests/test_ui_view_layers.py`'s 43 cases, which have no native
  counterpart yet since the layer stack itself isn't built.
- Run and require the macOS CI job, then address any Homebrew-specific
  compiler or deployment findings.

## Non-goals retained from Python

- No WPS binary geographical datasets.
- No checked support for antimeridian-crossing or polar display domains.
- No rotated lat/lon WRF/WPS projection support.
- Single-file internal times remain index-labelled when GDAL cannot expose
  `Times`; filename-based multi-file series can expose real labels.
