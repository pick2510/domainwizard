# Native C++ port status

The native implementation lives at the repository root (`src/`, `include/`,
`tests/`) on the `cplusplus` branch - it used to live under its own `cpp/`
subdirectory alongside the Python implementation, but was flattened up to
the top level once that implementation was removed and `cpp/` no longer
served any purpose. It is a C++20 / Qt 6 / GDAL application that reached
full feature parity with the original Python implementation (every Python
test file had a ported C++ counterpart) and now exceeds it in a few places
- see "WPS_GEOG binary dataset visualization" below. It is now the only
implementation in this repository - the Python source (formerly
`src/wrftools/`, `src/gis4wrf/`, `tests/test_*.py`) was removed once parity
was reached and active development moved fully to this codebase; it
remains recoverable from git history if ever needed. This document is kept
as the porting history and mostly still references the removed Python
modules, and the pre-flatten `cpp/src/`/`cpp/include/` paths, by name
throughout for that reason - it wasn't systematically rewritten after
either move, beyond this section.

## Completed

- CMake build, CTest integration, Catch2 tests (per-case discovery for the
  headless core suite), Linux install target, and macOS Homebrew build
  instructions / CI job. UI code lives in its own `wrftools_ui` library so
  `main_window`/`view_form`/`domain_form`/`tile_map_widget` are linkable from
  tests, not just the executable.
- Qt main window with shared map and native Domains / View / Convert tabs.
- **Convert tab**: a Qt6 GUI over
  [convert_geotiff](https://github.com/jbeezley/convert_geotiff) (public
  domain), a separate small tool for GeoTIFF <-> WPS geogrid conversion that
  is not part of the Python `wrftools`/GIS4WRF codebase this port otherwise
  tracks. Its conversion library (`src/convert_geotiff/`,
  `include/convert_geotiff/`) is vendored unmodified - GDAL-free, only
  linking `libtiff`/`libgeotiff` - as the new `convert_geotiff_lib` CMake
  target; only its GUI (originally FLTK) was ported, to
  `GeotiffConvertForm` (`geotiff_convert_form.hpp`/`.cpp`), replacing
  `Fl::lock()`/`Fl::awake()` with `QMetaObject::invokeMethod(...,
  Qt::QueuedConnection)` to marshal a background `std::thread`'s progress/
  completion back to the GUI thread. Follows this project's
  throwing-validator convention (`runConversion()` throws `UserError`;
  `startConversionFromSignal()`, wired to the button, catches and shows a
  `QMessageBox`) rather than the original's direct `fl_alert()` calls, so
  validation is testable without a blocking modal dialog. The library's own
  `std::filesystem::current_path()`-based output (tiles/index are written via
  relative paths) is scoped with an RAII guard that restores the prior
  working directory afterward, even on exception - the original process-wide
  `std::filesystem::current_path()` call in the FLTK version never restored
  it. 12 new Catch2 cases in `ui_tests.cpp` cover direction toggling,
  field-enablement, validation-throws-UserError, and two real end-to-end
  conversions (forward via `tests/fixtures/geotiff_convert/utm.tif`, and a
  forward-then-reverse round-trip) verified against real output files, not
  just "didn't throw". Four more cases exercise both directions against real
  geographic data pulled from NCAR's own
  [`geog_low_res_mandatory.tar.gz`](https://www2.mmm.ucar.edu/wrf/src/wps_files/geog_low_res_mandatory.tar.gz):
  `tests/fixtures/geotiff_convert/wps_soiltemp_1deg/` is one unmodified
  `soiltemp_1deg` geogrid tile (continuous/numerical, real deep-soil-
  temperature values in Kelvin) exercised in the reverse direction
  (geogrid -> GeoTIFF, checking every non-missing pixel lands in a sane
  physical range) and then forward again (GeoTIFF -> a new geogrid tile,
  checking the index stays `type = continuous`). There is no single-layer
  `type = categorical` dataset anywhere in that mandatory download to pull
  directly - NCAR's own landuse/soiltype sets ship as per-category
  *continuous* fraction layers instead (`tile_z` > 1, `type = continuous`
  even though `category_min`/`category_max` are present) - so
  `tests/fixtures/geotiff_convert/landuse_dominant_category.tif` is a small
  24x24 GeoTIFF derived from one (real, downloaded, not synthetic)
  `modis_landuse_20class_5m_with_lakes` tile by taking the per-pixel
  dominant category (argmax across its 21 fraction layers) - genuine
  MODIFIED_IGBP_MODIS_NOAH category codes, exercised forward (with the
  categorical checkbox and 21 categories, checking the written index says
  `type = categorical`/`category_max = 22`) and then reverse again,
  checking the round-tripped pixels match exactly except within a
  `tile_bdr`-wide margin of this fixture's own edge - a single 24x24 crop
  declared as its own whole tiled "domain" has no neighboring tile to
  source real border data from, unlike every actual WPS_GEOG dataset, so
  that margin is real-but-unrepresentative degenerate-domain-edge behavior
  in the vendored library rather than something this port's own code
  touches. `libtiff`/`libgeotiff` were already present in the
  portable Linux bundle as GDAL's own transitive dependencies (GDAL has a
  built-in GeoTIFF driver), so `build-portable-cpp.sh` needed no changes.
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
  green/yellow/red field validation styling, a **set-from-map-extent
  action**, and **domain outlines drawn on the shared map** (8-color
  palette cycled by stable WPS domain number, densified via `perimeter/200`
  segmentization so curved projections render correctly, zoom-to-domain
  over the densified outline). The set-from-*file*-extent action
  (`fileextent.py`, ported here as `file_extent.hpp`/`.cpp`) was removed by
  request - along with its now-unused `file_extent` module entirely, rather
  than leaving dead code behind - so this is a deliberate feature removal,
  not a gap. A `setActive` gate keeps field edits from yanking the camera while the
  View tab is on screen.
- WRF reader: GDAL NetCDF variable discovery, non-spatial-field filtering,
  time/level selection, nodata conversion, U/V-style destaggering, geographic
  bounds (now derived from the corrected projected geotransform, not raw
  corner samples). Fixes a real bug found against production WRF output (a
  120x120x64 Hong Kong domain run): some real wrfout files are NetCDF4/
  HDF5-backed, and a bare `GDALOpenEx(path)` lets GDAL's driver probing hand
  such a file to its generic HDF5 driver instead of the netCDF driver on a
  machine whose netCDF driver isn't built with HDF5 support - which moves
  per-variable attributes (MemoryOrder, description, units) off the
  subdataset's own metadata onto its first band's, and names subdatasets
  `HDF5:"path"://VAR` rather than `NETCDF:"path":VAR`, filtering out every
  variable and throwing "No displayable WRF/WPS variables were found" for
  an otherwise perfectly normal file. Fixed the way the Python reference
  already does it (`wrfreader._open_netcdf`, whose docstring names this
  exact failure mode): every open is forced through the `NETCDF:"path"`
  prefix so GDAL's netCDF driver is always the one used, never leaving the
  choice to driver-priority probing. Verified end-to-end against the real
  failing file - 164 variables now discovered (up from the 141 an earlier,
  more complex band-metadata-fallback version of this fix found, since
  forcing the driver also restores 3D fields like `T` with all 64 vertical
  levels, which the HDF5 driver's subdataset listing never exposed at all).
  Regression-tested with a NetCDF4/HDF5-backed copy of an existing fixture
  (`tests/fixtures/wrfout_multitime_nc4.nc`, `nccopy -k nc4`).
- WRF filename parsing and multi-file series reads, now with the real
  fast/eager path split from `wrfseries.py`: a series whose every file's
  valid time and single-timestep-per-file assumption can be trusted from
  its filename alone opens only its first file up front and stays lazy;
  one where any file can't be trusted that way (an unparseable name, or a
  first file with more than one internal timestep) opens everything eagerly
  and builds a real cross-file variable intersection and stepped time
  labels. Fixes a real crash: the previous version dereferenced
  `parseWrfFilename`'s `optional` before checking it succeeded, so a
  series containing an unparseable filename crashed instead of raising
  `UserError` or falling back.
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
  looping playback are wired per the selected layer. Level/soil/etc. row
  label now reflects the variable's actual extra dimension (`Vertical
  Level:`, `Soil Depth Layer:`, ...) instead of a hardcoded `Vertical
  level` for every case, mirroring `viewform.py`'s `_EXTRA_DIM_LABELS`. A
  manual range's max-must-exceed-min validation (`UserError`) is enforced
  and wired through `editingFinished` rather than every keystroke - firing
  on every intermediate value while typing minimum/maximum separately (or
  the instant "Auto range" is unchecked, before either field has been
  touched) would trip a spurious validation error and pop a blocking
  modal, found while porting `test_ui_view_layers.py`'s range test.
  `Show Info Overlay` checkbox added (`viewform.py`'s `show_info_check`
  had no native equivalent at all before).
- **`LayerRenderer` (`layer_renderer.hpp`/`.cpp`)**: the two-tier cache the
  View UI above actually renders through - a byte-bounded cache of warped
  (EPSG:3857, native-unit) slices keyed by (file, variable, time, level),
  and a count-bounded cache of colormapped images keyed by the slice key
  plus (colormap, vmin, vmax, unit), so a UI change that only touches
  opacity/visibility/colormap/range doesn't re-read and re-warp the GDAL
  subdataset. `renderLayer`/`colorizeWarped` (`raster_layer.hpp`) stay as
  the uncached path the cache itself and direct tests build on. Mirrors
  `wrftools.rasterlayer.LayerRenderer`'s cache tiers and byte/count budgets,
  now including its `stats` hit/miss counters (`Stats`/`stats()`) and
  `prefetch()`, added specifically to pin the caching contract in tests
  rather than only its externally visible effect.
- Native regression suite, now including cross-language pins: CRS/geotransform,
  warped-raster bounds, domain bboxes, and LANDUSE palette values are each
  checked against a value produced by running the Python reference on the
  same fixture, not just internal self-consistency. Real Qt widget-interaction
  tests for `DomainForm` (tree shape from a real namelist, selection via
  `setCurrentItem`) and `ViewForm` (a 4-file series collapsing to one file
  row, a 2-layer stack built and hidden via real checkbox clicks) in a new
  `wrftools_ui_tests` binary. Substantially ports `test_units.py`,
  `test_colormaps.py`, `test_colorbar.py`, `test_crs_datum.py`, and
  `test_wrfreader.py`, including three synthetic-fixture rejection tests
  (unsupported `MAP_PROJ`, rotated pole, a GDAL-openable non-WRF GeoTIFF)
  generated via `ncgen`/`gdal_translate` from real fixture CDL dumps
  (`tests/fixtures/wrf_unknown_projection.nc`, `wrf_rotated_pole.nc`,
  `not_a_wrf_file.tif`). Current local result: all 45 CTest entries pass.
- `RasterLayer` carries per-layer `tickCount`/`tickFormat`/`tickDecimals`,
  wired to a "Colorbar" group in `ViewForm` (tick format `auto`/`fixed`/
  `scientific`, decimals only enabled for the latter two). Deliberately
  excluded from `LayerRenderer`'s cache keys and routed straight to
  `updateColorbar()` rather than `refreshMap()` - a cosmetic legend tweak
  must not re-render or touch the slice/image cache, matching
  `wrftools.viewform`'s tick handlers.
- **Portable Linux artifact (`build-portable-cpp.sh`, `cmake/bundle_linux.cmake`)**:
  builds Release and bundles the executable, its full shared-library
  closure (`file(GET_RUNTIME_DEPENDENCIES)`), the Qt `platforms`/`tls`/
  `imageformats` plugins (dlopen'd, so invisible to that closure - resolved
  via a second `GET_RUNTIME_DEPENDENCIES` pass with `MODULES` so a plugin's
  own deps, e.g. the TLS backend's libssl/libcrypto, aren't missed), and
  GDAL/PROJ's data files, then `patchelf --set-rpath`s everything to
  `$ORIGIN`-relative paths. Requires `patchelf` (`apt install patchelf` on
  Debian/Ubuntu) - nothing else outside apt. Verified end-to-end on this
  machine: the bundle launches and its GDAL/PROJ data perform a real CRS
  transform with `LD_LIBRARY_PATH`/`GDAL_DATA`/`PROJ_DATA` pointed only at
  itself, no system Qt/GDAL/PROJ in scope. Deliberately does *not* bundle
  libc/libstdc++/libgcc_s/the dynamic loader (tied to the target machine's
  kernel/ABI) - promises portability across a comparably-recent-or-newer
  glibc than the build machine, not literally any glibc the way
  `build-portable.sh`'s Docker/manylinux pipeline does for the Python side.
- **Portable macOS artifact (`build-portable-macos.sh`)**: builds Release
  and bundles the `MACOSX_BUNDLE` app (`dist/wrftools.app`) via Qt's own
  `macdeployqt` (Qt frameworks/plugins) followed by `dylibbundler`
  (everything else non-system - GDAL, PROJ, and their own transitive
  deps), then copies GDAL/PROJ's data directories into
  `Contents/share/{gdal,proj}` - the same relative layout
  `configureGdalData()` already looks for beside the executable (which
  lives at `Contents/MacOS/wrftools` inside a bundle), so no `GDAL_DATA`/
  `PROJ_DATA` environment setup is needed at run time. Zips the result to
  `dist/wrftools-macos.zip`. Unlike `bundle_linux.cmake` (a from-scratch
  `file(GET_RUNTIME_DEPENDENCIES)` walk, since Linux has no macdeployqt
  equivalent), this leans on the two tools that are the de facto standard
  for this exact job on macOS. **Not verified end-to-end** - no macOS
  machine available to build or run this from within the current working
  environment; CI (which does have one) is the actual first real test of
  it, and this note should be revisited once a macOS CI run has confirmed
  the artifact actually launches and finds its own GDAL/PROJ data.
- **CI now uploads both as GitHub Actions build artifacts**
  (`.github/workflows/cpp.yml`, `wrftools-linux`/`wrftools-macos`) on
  every push/PR to `cplusplus`, once - not instead of - the existing
  Debug build/test steps pass; the portable-bundle step is a second,
  separate Release/`BUILD_TESTING=OFF` configure+build, matching what
  running either script by hand already does.

## Still required for feature parity

### Packaging and verification

- Every Python test file now has a ported counterpart except for one
  case, since fixed (see below): `test_core_domain_tree.py`/
  `test_ui_domain_tree.py`'s sibling-tree round-trip, renumbering,
  outline-distinguishability, and out-of-bounds-child-via-field-edit
  cases are ported (bbox round-trip tests for both the sibling and
  linear-chain fixtures added to `core_tests.cpp`, the rest to
  `ui_tests.cpp` with new `DomainForm` test-only accessors:
  `addChild()`/`removeSelected()`/`applySelectedDomainFields()` made
  public, `paddingLeftField()`/`paddingBottomField()` added).
- **"Add Root Domain" blank-slate flow**: `DomainForm` now matches
  `domainform.py` exactly instead of always requiring `setProject()`
  first. The constructor initializes `project_` to an empty (not
  `nullopt`) `WpsProject` - mirrors `Project.create()` - and
  `addChild()` (wired to the same button as ever) now creates the root
  domain itself (lat-lon, a 0.1x0.1 degree cell, a 10x10 grid, centered
  on 0N/0E - `domainform.py`'s exact defaults) when the domain list is
  empty, instead of doing nothing. The button's own text/enabled state
  now tracks this too (`addDomainButton()`/`removeDomainButton()`
  accessors added): "Add Root Domain" (always enabled) with no domains,
  "Add Child Domain" (needs a selection) once one exists - mirrors
  `_update_panel_visibility`'s `add_domain_button` wiring exactly. This
  closes `test_ui_domain_tree.py::test_build_siblings_through_ui`, the
  one Python domain-tree case noted above as unported by design; it now
  has a direct C++ port (`"building a sibling tree from scratch through
  addChild()"`).
- Port the remaining Python tests (197 total in the Python suite; the native
  suite has grown to 76 CTest entries). `test_wrfseries.py` (20 cases),
  `test_tilemap_overlays.py` (13 cases), `test_export.py` (2 cases, plus an
  existing widget test already covering the readable-PNG case), and
  `test_rasterlayer.py` (33 cases - cache-tier hit/miss behavior pinned via
  `LayerRenderer::Stats`, effective-range/manual-override/unit-conversion,
  categorical legend data through the real `render()` path, series
  rendering, and both eviction policies) are now ported. Two Python cases
  in that last file have no C++ equivalent by design: `render()` opens its
  file on demand rather than distinguishing "not yet opened" from "open it
  now" (`WrfSourceRegistry::open` is idempotent), and `interpolate` is
  applied by `ViewForm` when building the map overlay, not part of
  `RenderedRaster`. `test_ui_view_layers.py` (43 cases) is now ported too,
  covering file/layer opening and series grouping, the properties panel
  (variable/time/level/colormap/units/opacity/range/interpolate,
  categorical auto-detect and manual override), the colorbar
  (shown/hidden by selection and visibility, follows selection, tick
  controls), layer add/remove/reorder, closing a file, zoom-to-layer and
  auto-zoom-on-first-layer, playback (start/stop/wrap/stops-on-reselect),
  and the info overlay - found and fixed three real bugs along the way
  (`addLayerButton` staying permanently disabled until a layer-tree
  rebuild happened to run; the layer tree listening for `currentItemChanged`
  instead of `itemSelectionChanged`, so `clearSelection()` didn't hide the
  colorbar; and a range-validation deadlock, all detailed above). Every
  Python test file now has a fully ported counterpart, with no remaining
  deliberate architecture differences - see the "Add Root Domain" bullet
  above for the last one, since closed.
- Run and require the macOS CI job, then address any Homebrew-specific
  compiler or deployment findings.
- Two native-only additions beyond Python parity: the movable colorbar
  (`TileMapWidget::legend_`) now has a drag-resize handle at its bottom-right
  corner (`legendResizeHandleRect()`/`legendScale()`, `dragTarget() ==
  "legend-resize"`) - Python's colorbar is movable but not resizable, so
  there is no source to port here. And the info overlay
  (`ViewForm::updateColorbar`) now packs in more than the variable/time it
  showed before: it falls back to the file's own filename-parsed valid time
  (`formatWrfTimestamp`, exposed from `wrf_series.hpp`) rather than "Step N
  of M" when a layer isn't part of a series, and appends the visible
  level/soil/etc. row (when shown) and the rendered value range.
- A third native-only addition: domain outlines are directly draggable on
  the map (Python's `domainform.py` has no map-driven repositioning at all -
  only the properties panel and the two "Grid Extent Calculator" buttons
  change a domain's placement). `TileMapWidget::setDraggableVectorOverlayGroup`/
  `setOverlayDragHandlers` keep the map itself generic (it hit-tests
  whichever named vector-overlay group is marked draggable and reports back
  a polygon index plus the lon/lat under the cursor - it has no idea what a
  "domain" is); `DomainForm` is the only caller, arming it for the
  `"domains"` group only while its own tab is active
  (`setActive`/`setDraggableVectorOverlayGroup(active_ ? "domains" : {})`),
  so a click on the map while the View tab owns it always pans, never
  repositions a domain sitting underneath, and View's own raster layers
  (a different overlay group entirely) are never draggable regardless.
  Dragging a root moves its Center Point (continuous degrees); dragging a
  nested domain moves its Position within Parent (whole parent-cell counts,
  clamped at 0, converted through the project's own CRS rather than a flat
  degree/pixel ratio so it stays exact under every projection) - both
  select the dragged domain and refresh the properties panel on every
  mouse-move tick, per the request that drove this ("update all values in
  the properties when they are moved around"). Moving any domain
  automatically carries its descendants along for free: `fillDomains()`
  always derives a child's bounds from its parent's *current* bounds plus
  the child's own fixed padding, so nothing here has to touch descendants
  explicitly - dragging a root's whole subtree is a consequence of that,
  not special-cased code.

  Two follow-ups landed with the first drag-and-drop pass, from real usage
  feedback ("big latency", "no frame around the moving object"):
  - **Latency**: `Crs::toXy`/`toLonLat` (`crs.cpp`) were rebuilding a whole
    `OGRSpatialReference` pair and `OGRCoordinateTransformation` from the
    proj4 string on *every single call* - fine in isolation, but
    `domainoverlay.cpp`'s `densifiedRing` calls one of them up to ~200
    times per domain outline, reusing the *same* `Crs` instance for every
    domain and every vertex. Every mouse-move tick during a drag re-ran
    that whole densify-and-reproject pass, so the per-call GDAL/PROJ setup
    cost (parsing, `proj.db` lookups) was paid hundreds of times per frame.
    `Crs` now lazily builds its `OGRCoordinateTransformation` pair once
    (a `shared_ptr<TransformCache>`, so copies of the same `Crs` share it
    too) and reuses it for the instance's lifetime - a real perf fix, not
    just a drag-specific one: it also cut the whole native CTest suite's
    wall time from ~24s to ~2.4s, since `core_tests`/`widget_tests` exercise
    plenty of repeated transforms of their own.
  - **Precision**: the polygon actually being dragged now redraws on top of
    everything else with a white-halo + black-dashed outline
    (`TileMapWidget::overlayPath`, used by both the normal vector-overlay
    paint pass and this highlight pass so they always trace the identical
    outline) - a plain colored outline was easy to lose against similarly
    colored basemap tiles or an overlapping domain while dragging.

  A fourth native-only addition: domains are also resizable on the map, not
  just movable. `VectorOverlay` gained an optional `handles` field (0 or 4
  lon/lat corner points, SW/SE/NE/NW) - `computeDomainOverlays`
  (`domain_overlay.cpp`) fills it in from the domain's own authoritative
  `Bounds2D`, not anything inferred from the densified/curved outline ring.
  `TileMapWidget::setOverlayResizeHandlers` mirrors the move hooks exactly
  (`hitTestOverlayHandle`, checked *before* the body hit test so a handle
  sitting on the polygon's own edge always wins; handle squares are always
  drawn for the draggable group, like the legend's own resize handle, not
  just while resizing). `DomainForm::onDomainOverlayResize{Start,Move,End}`
  capture the corner diagonally opposite the dragged one once, at drag
  start, as a fixed anchor in the domain's own CRS (its `Bounds2D` changes
  every move, so re-deriving the anchor from it every tick would let it
  drift) - a root then recomputes `columns`/`rows` from `width/height /
  dx`/`dy` and its Center Point as the midpoint of the anchor and the
  dragged corner (true for any rectangle's diagonal), while a nested domain
  recomputes `columns`/`rows` the same way plus `paddingLeft`/
  `paddingBottom` from where its new (possibly-shrunk) box's own corner now
  sits relative to its parent's bounds - the resolution/cell size itself
  never changes, matching a manual Grid Extent field edit.

- **System light/dark theme following** (`theme.hpp`/`.cpp`, wired from
  `main_window.cpp`): another native-only addition - the Python app never
  had any theme handling at all. On macOS/Windows, Qt's native style
  already tracks OS dark mode on its own with zero extra code, so this
  only actually *changes* anything on Linux (which needs an explicit
  palette swap; without one, Qt renders a fixed light `Fusion`-ish look
  regardless of the desktop's dark-mode setting whenever there's no
  GTK/qt6ct/xdg-desktop-portal integration reporting it). `theme.hpp`
  defines its own `ColorScheme` enum (Light/Dark/Unknown) rather than
  using `Qt::ColorScheme` directly, deliberately: that type and
  `QStyleHints::colorScheme()`/`colorSchemeChanged` only exist from Qt
  6.5, and Ubuntu 24.04 - what the Linux CI job's `apt`-installed
  `qt6-base-dev` actually is - ships 6.4.2, so requiring 6.5 outright
  would have broken that job. The real `Qt::ColorScheme` is touched in
  exactly one place, `main_window.cpp`, mapped to `wrftools::ColorScheme`
  behind `#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)`; built against 6.4,
  that code is simply absent (verified by compiling `main_window.cpp`
  with that branch stripped, not just reading it) rather than a compile
  error, and System-preference theming still works, just without live
  OS-toggle tracking. `darkPalette()` is the well-known Fusion
  dark-palette recipe (light gray text on a dark gray window/base, not
  pure white, with a distinct dimmer shade for disabled widgets);
  `applyColorScheme(app, scheme)` swaps to Fusion + that palette for
  `Dark` and back to whatever the platform's own native style/palette
  were (captured once, on its very first call, before anything is ever
  touched) for `Light` or `Unknown`.

  Real-world GNOME testing found `QStyleHints::colorScheme()` itself
  unreliable there: it depends on either a QPA platform theme plugin or a
  reachable xdg-desktop-portal D-Bus service, and plenty of real GNOME/Qt
  combinations have neither wired up, so it kept reporting Light/Unknown
  on an actually-dark GNOME desktop. `resolveColorScheme(reported)` now
  sits between `colorScheme()` and `applyColorScheme()`: it shells out to
  `gsettings get org.gnome.desktop.interface color-scheme` (present on
  essentially every GNOME desktop, no portal/plugin dependency) and
  prefers that answer whenever it's reachable, falling back to `reported`
  unchanged everywhere else (macOS, Windows, non-GNOME Linux, or a GNOME
  desktop with no `gsettings` binary) - kept as a separate function
  precisely so `applyColorScheme` itself (the style/palette decision)
  stays a pure, directly-testable function of an already-resolved scheme,
  with the environment-dependent shelling-out isolated to
  `resolveColorScheme` and not unit tested itself (no portable way to
  control a real desktop's `gsettings` output from a test). One caveat
  from the same root cause: since GNOME's own `colorSchemeChanged` never
  fires there either, a mid-session GNOME dark-mode toggle isn't picked up
  live - only a fresh launch re-resolves it.

  Even `resolveColorScheme`'s GNOME fallback turned out not to be enough
  on its own in practice - one real GNOME desktop had an explicit dark
  theme/mode set (confirmed by the user) while `gsettings get
  org.gnome.desktop.interface color-scheme` still reported `'default'`
  and `gtk-theme` reported plain `'Adwaita'`, not a `-dark` variant, so
  automatic detection had no reliable signal to key off at all on that
  system. Rather than chase more detection heuristics, `MainWindow` now
  has an **Options > Theme** menu (System/Light/Dark, exclusive via
  `QActionGroup`) as a manual override - `System` keeps following the OS
  via `resolveColorScheme`/`applyColorScheme` (live, through `main_
  window.cpp`'s own `colorSchemeChanged` connection, only while `System`
  is the checked action, and only compiled in when built against Qt 6.5+ -
  see the CMake-minimum note above), while `Light`/`Dark` apply
  `wrftools::ColorScheme::Light`/`Dark` directly (no Qt-6.5 dependency)
  and stick regardless of whatever the OS reports afterward. The choice
  is persisted via
  `ThemePreference`/`themePreference()`/`setThemePreference()`
  (`theme.hpp`/`.cpp`) through a default-constructed `QSettings()` (using
  the organization/application name `main.cpp` sets on `QApplication`),
  so it survives a restart - `main.cpp` itself no longer applies any
  theme at startup, since that decision now depends on the persisted
  preference and so belongs entirely to `MainWindow`, which reads it as
  early as possible in its own constructor.

## Reproject tab (exceeds Python)

A new "Reproject" tab - Python had no equivalent - reprojects a wrfout file,
or a whole wrfout series (`groupWrfPaths`), to an arbitrary EPSG code as
CF-1.7 conformal NetCDF. `include/wrftools/reproject.hpp`/`src/reproject.cpp`
hold all the logic in `wrftools_core`, independent of Qt.

- **Variable selection is exactly `WrfFile`'s own filter**: only variables
  whose `MemoryOrder` starts with `"XY"` are ever offered (already the
  "2D and 3D only" rule the feature needs - Z-only fields like `ZNU`/`ZS`
  are excluded automatically, same as the View tab).
- **Horizontal destaggering** comes for free from `WrfFile::read`, which
  already averages `U`/`V`-style staggered edges onto the mass grid.
  **Vertical destaggering** is new: a variable whose extra dimension ends in
  `_stag` (except `soil_layers_stag`, which despite its name holds soil
  layer midpoint depths, not a staggered grid) has adjacent levels averaged
  and collapses onto the *same* dimension name/length the model's
  unstaggered variables already use - e.g. `W`'s `bottom_top_stag` (65
  levels) becomes 64 levels under `bottom_top`, the dimension `T`/`P`/`U`
  already share, rather than a separate output dimension.
- **The destination grid is computed once per run** (`computeDestinationGrid`,
  built on a new `suggestWarpGrid` in `warp.hpp`/`.cpp` - the same
  `GDALSuggestedWarpOutput` query the display warp path already made
  internally, now exposed uncapped since an export must not hit the display
  path's `kMaxWarpDimension` downsampling heuristic) and reused for every
  variable/level/timestep of every output file in the run, so a per-file
  export is pixel-aligned across files. Every slice goes through the
  existing `warpToGrid`, with resampling forced to nearest for any variable
  `WrfFile` already tags with a `categoryScheme` (LU_INDEX, IVGTYP, ...),
  regardless of what the user picked - bilinear/average would blend
  category codes into meaningless fractional values.
- **Follow-up (2026-08-28): a real datum-shift bug, found via a user
  report** (a QGIS overlay of a reprojected EPSG:2326 - Hong Kong 1980 Grid
  - file against OpenStreetMap tiles showed a visible offset). Root cause:
  `WrfFile`'s own source CRS is built on WRF's modeling sphere
  (`a=b=6370000`, see `crs.hpp`) with **no datum linkage to WGS84 at all** -
  deliberate, so the app's own internal geometry (map display, LCZ
  pipeline) never picks up a spurious shift, and the sphere's own lat/lon
  values are, by WRF's modeling convention, real-world WGS84 coordinates
  (already confirmed elsewhere in this codebase to agree with geogrid.exe's
  own `XLAT_M`/`XLONG_M` to within a few metres). Warping straight from
  that undefined-datum sphere to an external target CRS is harmless when
  the target's own geographic base **is** WGS84 (true of EPSG:4326 itself,
  every UTM zone, Web Mercator, ...) - PROJ falls back to an identity/
  no-shift, which happens to be exactly correct there. It silently gives
  the **wrong** answer, by hundreds of metres, when the target uses a
  genuinely different (usually legacy/local) datum: PROJ has a real,
  published WGS84->HK1980 transformation for EPSG:2326, but never finds it,
  because the source CRS's datum was never asserted to *be* WGS84 - it just
  no-ops instead of erroring. Confirmed empirically (point transform, not
  guessed): the WRF domain centre transformed straight from the sphere to
  EPSG:2326 landed ~300 m from the same point transformed through real
  WGS84. A `+towgs84=0,0,0` "identity shift" hack on the sphere was tried
  and rejected - it makes things *worse* (~15 km off), because a null
  Helmert shift is done in geocentric XYZ space using the source ellipsoid's
  own radius, and the sphere's radius (6370000 m) doesn't match WGS84's
  (6378137 m) at all.
  Fixed with an explicit **two-stage warp**, applied only when the target's
  geographic base genuinely differs from WGS84 (`OGRSpatialReference::
  IsSameGeogCS`, checked once per run in `targetSharesWgs84Datum`) - the
  common case (UTM, EPSG:4326, Web Mercator, ...) stays a single warp,
  unchanged: first warp from the source sphere to an intermediate grid on
  that *same* sphere in geographic (lon/lat) space (an exact,
  distortion-free unprojection, since source and intermediate share one
  identical GEOGCS), then **relabel** that intermediate's CRS as literal
  EPSG:4326 (legitimate given the accuracy note above) and warp again from
  that properly-labelled WGS84 raster to the real target CRS, where PROJ
  now finds and applies the genuine datum transformation
  (`suggestWarpGridDatumSafe`/`DatumSafeSuggestion` in `reproject.cpp`).
  Applied consistently to **both** the destination grid's own placement
  (`computeDestinationGrid`) and the per-slice data warp (`runReproject`'s
  `warpSlice` lambda) - the first attempt at this fix only corrected the
  data warp and left the grid's own coordinate values computed via the old
  single-stage (buggy) path, which would have positioned the CF coordinate
  axes ~300 m away from the data actually resampled into them; both must
  go through the same datum-safe suggestion or they silently disagree.
  Pinned by a regression test (`core_tests.cpp`) comparing the reprojected
  EPSG:2326 output's own NW corner against an independent, real WGS84->
  EPSG:2326 point transform: measured directly against both code paths
  while writing the test, the pre-fix warp lands ~170-222 m off and the
  fixed one lands within ~30 m (residual pixel-corner/resampling noise) -
  the test's 100 m threshold sits well clear of both, catching a
  regression back to either failure mode.
- **Output structure**: real `x`/`y` (projected) or `lon`/`lat` (geographic)
  coordinate variables, a CF `time` coordinate (`seconds since <first
  record>`, parsed from each input's own `Times` variable, not the
  filename, so it works for both a wrfout series and a multi-record single
  file), the verbatim `Times` char variable kept alongside it, and a `crs`
  grid-mapping variable built from `OGRSpatialReference::exportToCF1()` -
  which required bumping `find_package(GDAL 3.6 REQUIRED)` upward (see the
  2026-08-28 CI follow-up below for the actual minimum and why Linux CI
  needed a dependency-install fix, not just the CMake bump, to get there).
  One non-obvious API detail: `exportToCF1`'s first out-parameter is GDAL's
  *recommended variable name* for the mapping ("crs"), not the
  `grid_mapping_name` attribute value itself - that comes back inside the
  key/value list instead, keyed literally `"grid_mapping_name"`. Getting
  this backwards silently produces a `grid_mapping_name` of `"crs"`, valid
  CF syntax but semantically wrong; caught by a test that resolves EPSG:4326
  in isolation (without any prior `GDALAllRegister()` call from another
  test) and asserts on the actual string, not just that *something* was set.
- **Global attributes**: everything from the source is copied forward
  *except* the ones that describe the WRF projection/grid the file no
  longer has (`MAP_PROJ`, `TRUELAT1/2`, `STAND_LON`, `CEN_LAT/LON`, `DX`,
  `DY`, the `*_PATCH_*`/`*_GRID_DIMENSION` family, ...) - those are
  **renamed with a `WRF_` prefix**, not dropped: leaving them under their
  canonical name would be actively misleading (this app's own
  `WrfFile::buildWrfCrs`, or any other WRF-aware reader, would happily
  reconstruct the *old* grid from them and place the reprojected raster in
  the wrong spot), but the provenance is real and worth keeping.
- **`NetcdfFile` gained**: `create()` (a brand-new file, vs. `open()`'s
  existing-file-only contract), `writeFloatSlice`/`readFloatSlice`/
  `writeDoubleSlice` (hyperslab I/O - a merged multi-hundred-file series is
  far too large to hold one variable's whole array in memory, which is what
  every prior write method on this class required), `readText`/`writeText`
  (`Times` is `NC_CHAR`, which the existing float/double/int read/write
  methods can't express), and `attributes()` (enumerate every attribute on
  a variable/global, for the "copy everything except these names" global
  attribute pass above). `defineVariable` gained optional `chunkSizes`/
  `deflateLevel`/`fillValue` parameters rather than separate
  `setChunking`/`setCompression` methods, after hitting a real netCDF-4 API
  restriction: `nc_def_var_chunking`/`nc_def_var_deflate`/a `_FillValue`
  attribute must all be set in the *same* `nc_redef`/`nc_enddef` episode as
  `nc_def_var` itself, and every other method on this class (matching its
  existing convention) pays its own redef/enddef round-trip per call - a
  separate `setChunking()` called afterward is always one `enddef` too late
  ("Attempt to define var properties... after enddef").
- **Runs out-of-process, not on the GUI thread and not on a background
  `std::thread`.** `LczForm`'s own class comment documents a real,
  reproduced deadlock when netCDF-C/GDAL/HDF5 is touched from a second OS
  thread after the GUI thread already has (a non-threadsafe libhdf5 build) -
  a `std::thread` is therefore unsafe for this too, and a merged
  multi-hundred-file export is far too long to accept blocking the GUI
  (the LCZ tab's accepted tradeoff for its much shorter pipeline). Instead,
  `ReprojectForm::runReproject()` launches a small separate executable,
  `wrftools_reproject_worker` (`src/reproject_worker.cpp`, linked only
  against `wrftools_core` - no Qt Widgets, no GUI), as a `QProcess`: a job
  description is written to a temporary JSON file, and the worker's stdout
  is parsed line by line (`PROGRESS <completed> <total> <message>` /
  `DONE <path>` / `ERROR <message>`) to drive the progress bar and log view
  through Qt's ordinary event loop. `runReproject()` itself validates the
  form and starts the process, then returns immediately - it does not
  block, and Cancel just kills the process.
- Fixtures under `tests/fixtures/reproject/` are carved (via `ncks`, not
  synthesized) from a real WRF 4.7.1 run over Hong Kong, keeping real
  geometry/attributes/projection at a committable size.
- **Follow-up (2026-08-28): tab panel widened for the Reproject tab's own
  field count.** The Reproject tab's natural width (its EPSG/pixel-size/
  extent/variables/output-directory controls) came out wider than every
  earlier tab, and `main_window.cpp`'s original `tabs->setMinimumWidth(340)`
  + `splitter->setSizes({360, 940})` were sized for the LCZ tab (the
  previous widest, ~440px `sizeHint`) - confirmed visually via an offscreen
  Qt probe (`QT_QPA_PLATFORM=offscreen`, grabbing each tab to a PNG, no
  X server needed), which showed the Reproject tab's Browse buttons,
  checkbox labels, and Run/Cancel row all running off the visible edge,
  reachable only via a horizontal scrollbar. Fixed two ways: (1)
  `ReprojectForm`'s pixel-size and extent fields, previously one
  `QFormLayout` row per axis (6 rows), are now paired X/Y on one row each
  (3 rows) with a small "X"/"Y" sub-label ahead of each field - this alone
  dropped the tab's own natural width from ~504px to ~442px; checkbox
  labels were also shortened to fit ("Always use nearest-neighbour for
  categorical variables" -> "Nearest-neighbour for categorical variables").
  (2) `MainWindow`'s tab panel minimum width raised 340 -> 460 and the
  splitter's initial split 360/940 -> 480/920 (default window 1300x800 ->
  1400x800, so the map doesn't lose net space) - comfortably clears every
  tab's own `sizeHint` (LCZ and Reproject both land around 440-442px) plus
  the scroll area's frame/scrollbar reservation, verified by re-running the
  same offscreen probe until no tab showed a horizontal scrollbar.
- **Follow-up (2026-08-28): map-drawn Area of Interest, cropping the
  reprojection.** `ReprojectForm` now takes the shared `TileMapWidget*`
  (constructor signature changed, `ReprojectForm(TileMapWidget*, QWidget*)`,
  matching `DomainForm`/`ViewForm`) instead of being map-independent. Loading
  a wrfout file or series draws its footprint - `WrfFile::geographicBounds()`,
  the same real WGS84 extent already used by the datum-shift fix above - as a
  lightly shaded, filled rectangle on the map (a new `VectorOverlay::fill`
  field, transparent by default so no existing caller/overlay changes
  appearance; `TileMapWidget::paintEvent` now fills a closed overlay's path
  before stroking it, same order as the legend/info boxes). A second
  rectangle, initially matching the domain exactly, sits on top with corner
  handles: dragging a handle resizes it (anchored on the diagonally opposite
  corner, exactly `DomainForm`'s own resize convention - see
  `kOppositeCorner` in `reproject_form.cpp`, deliberately mirroring
  `domain_overlay.cpp`'s identical `kOppositeOf`), and dragging its body
  moves it - reusing `TileMapWidget`'s existing drag/resize-handler and
  corner-handle machinery wholesale rather than adding a new "rubber-band
  draw" interaction mode. No new core logic was needed for the actual
  cropping: `GridOverride::extent` (target-CRS bounds) already existed and
  `computeDestinationGrid`/`runReproject` already restrict the destination
  raster to it, so GDAL's warp naturally only computes pixels inside the
  selection - the AOI rectangle is purely a visual way to fill the existing
  Extent min/max fields, not a separate code path. Every drag/resize move
  re-derives those fields via `Crs::wgs84().transformBbox(aoiBounds,
  targetCrs)` (`targetCrs` built from `describeTargetCrs(epsg).wkt`, so it
  tracks whatever EPSG the user has typed, including a change mid-edit via
  the field's `editingFinished`); a "Reset area of interest to full domain"
  button clears them back to "auto" rather than writing the rectangle's own
  coordinates, since GDAL's suggested grid for the untouched full domain is
  generally tighter than this rectangle's plain WGS84 bounding box.
  Manually typing the Extent fields still works exactly as before - the
  rectangle is a convenience on top of them, not a replacement.
  One correctness fix was needed to make any of this safe: `MainWindow`'s
  `QTabWidget::currentChanged` handler compared `tabs->widget(index)`
  against the raw form pointers (`domainForm`, and the new `reprojectForm`),
  but every tab is actually added via `scrollWrap(form)` - `tabs->widget()`
  returns the wrapping `QScrollArea`, never the form itself, so that
  comparison was always false. Latent before this change (its only visible
  effect was `DomainForm::setActive(true)` never actually firing from a tab
  switch, silently relying on that class's own `active_{true}` default
  member value matching the Domains tab's initial selection), it became
  load-bearing once a second form needed the same one-drag-handler-slot-per-
  map arbitration - `TileMapWidget` stores a single set of drag/resize
  callbacks, not one per group, so `DomainForm` and `ReprojectForm` must each
  re-claim it in their own `setActive(true)` (both now do) and the tab
  bar must actually tell them which one that is. Fixed by comparing against
  the `QScrollArea*` returned from each `scrollWrap()` call instead.
- **Follow-up (2026-08-28): the AOI rectangle can only crop the domain, never
  extend past it** - there's no data out there to crop, so letting the
  rectangle stray outside the footprint would silently ask GDAL for a grid
  extent partly outside the source raster (harmless in itself - the warp
  just leaves those cells NaN/fill-valued - but pointless and confusing to
  offer). `onAoiResizeMove` now clamps the dragged corner itself into the
  domain bounds (`clampToDomain`, plain `std::clamp` per axis) before
  computing the new min/max, so a corner handle simply stops at the domain
  edge instead of pulling the rectangle past it - the anchor corner is
  always already inside the domain (an invariant `aoiBoundsLonLat_` never
  violates), so clamping just the moving corner is sufficient for an
  axis-aligned box. Body drags need a different clamp
  (`clampRectToDomain`): clamping each corner of the translated rectangle
  independently would shrink/distort it, so instead the *translation* is
  clamped - shifted back just enough that the whole rectangle re-enters the
  domain, preserving its size - a wall the rectangle stops against rather
  than a rubber band that snaps its shape.
- **Follow-up (2026-08-28): Linux CI build failure - `exportToCF1` actually
  needs GDAL >= 3.9, not 3.7 as originally assumed**, plus two calls to a
  GDAL API that needs newer still. CI's Linux job (`libgdal-dev` via plain
  `apt-get` on `ubuntu-latest`/24.04 "noble") failed with `error: 'class
  OGRSpatialReference' has no member named 'exportToCF1'` and `error: no
  matching function for call to 'OGRSpatialReference::exportToWkt()'` -
  Ubuntu 24.04's own packaged `libgdal-dev` is 3.8.4, and `exportToCF1`
  only landed upstream on 2024-01-19 (commit `d0d5db642f`), which shipped in
  GDAL 3.9.0 (2024-05) - after 3.8.4 branched off, so no Ubuntu 24.04 point
  release will ever carry it via the plain archive. The earlier "3.7"
  minimum (see the bullet above) was never actually verified against a real
  build on that GDAL version; it happened to work locally and on macOS/
  Windows CI (Homebrew and MSYS2 both ship far newer GDAL already) which
  masked the gap until this branch first hit Linux CI. The zero-argument
  `std::string OGRSpatialReference::exportToWkt()` convenience overload used
  in two places (`wgs84PivotWkt()`, and building `TargetCrsInfo::crsWkt2`)
  is *also* newer than 3.8.4's headers provide - fixed independently of the
  version bump by switching both call sites to the plain `exportToWkt(char**
  [, const char* const* options])` form, which has existed since GDAL 2.x
  and needs no minimum-version reasoning at all (the `crsWkt2` call now also
  passes `FORMAT=WKT2` explicitly, since the no-arg overload's actual
  default format was never verified either - matching what that field's own
  name always claimed). `CMakeLists.txt`'s `find_package(GDAL 3.6 REQUIRED)`
  is now `find_package(GDAL 3.9 REQUIRED)` - the real, verified floor - and
  `cpp.yml`'s Linux dependency step now adds the `ubuntugis-unstable` PPA
  (3.11.x for noble) before `apt-get install`, so `libgdal-dev` resolves to
  a new-enough version there too; macOS/Windows CI needed no GDAL-version
  change (Homebrew/MSYS2 already ship newer than 3.9) - but getting Linux CI
  green for the first time surfaced two more, unrelated, Windows-only
  failures that this same push also fixes:
  - Three of the new `NetcdfFile` tests (`create`/`writeFloatSlice`/
    `readText`+`writeText`) failed with "cannot remove: The process cannot
    access the file because it is being used by another process" - the same
    open-handle-blocks-delete issue `9f90b83` already fixed elsewhere in
    this file, just not yet applied to these three, added afterward on the
    `reproject` branch. Same fix: scope the `NetcdfFile` in its own `{ }`
    block so it closes via RAII before the trailing `std::filesystem::
    remove`.
  - The end-to-end `ReprojectForm` UI test (the one real `wrftools_
    reproject_worker` subprocess run) hit `waitWhileRunning`'s internal 30s
    deadline without finishing, even though the worker was genuinely still
    working (its accumulated GDAL warnings, dumped as Catch2 `INFO` context
    on the failure, show real per-file progress, not a stall) - Windows CI
    process-spawn + fresh-DLL-load overhead (Qt/GDAL/netCDF/PROJ, no warm
    page cache) plus three real GDAL warps just needs more than 30s there.
    Raised to 90s, and `cpp.yml`'s Windows `Test` step's own `ctest
    --timeout` raised 120 -> 240 to match - the whole `wrftools_ui_tests`
    binary (one `add_test()`, not per-case `catch_discover_tests` - see
    `CMakeLists.txt`) had already measured at ~115s on Windows even while
    this one case was failing early, leaving 120s no real margin once it's
    allowed to actually finish. The Windows-only "Full LCZ pipeline" timeout
    seen in the same run needed no source change - it inherits the same
    ctest `--timeout` and was very likely just as starved for margin.
  A second CI run confirmed both of those (all `NetcdfFile` tests and the
  LCZ pipeline timeout green on Windows), but the `ReprojectForm` case
  itself still failed the same way even at 90s - and the accumulated
  `PROGRESS` messages captured in the failure's own `INFO` context showed
  it had already finished writing all 3 real timesteps (`wrfout_d03...:
  T2` logged three times) before going quiet, i.e. it was hanging in
  `runReproject`'s final `out.close()` (a plain `nc_close`, `src/
  reproject.cpp:676`) - after the expensive GDAL work was already done, not
  during it, so a longer wait was never going to fix it. The real cause was
  almost certainly the CI Windows Defender exclusions predating this
  feature: `$env:GITHUB_WORKSPACE` and the MSYS2 install dir were excluded,
  but not `$env:TEMP` - a different root entirely (`C:\Users\runneradmin\
  AppData\Local\Temp`) - which is exactly where `QTemporaryDir`/
  `std::filesystem::temp_directory_path()` write this test's real,
  GDAL-produced, chunked+compressed `.nc` output; real-time scanning of a
  freshly-closed HDF5 file blocking its own close/fsync is a documented
  class of Windows-runner-only stall. Fixed by adding `$env:TEMP` to
  `cpp.yml`'s existing Defender-exclusion step, plus the worker and test
  executables themselves to `ExclusionProcess` as a second layer.
  Neither of those actually fixed it, though - confirmed by a deliberate
  one-off diagnostic (`waitWhileRunning(ReprojectForm&)` bumped to 300s,
  `cpp.yml`'s Windows `ctest --timeout` to 420 to match): the test still
  hung identically, now measured at the full 300s, settling that this was
  never a matter of needing more time. The real cause: HDF5 (which every
  netCDF-4 file goes through) can hang indefinitely - not just run slowly -
  in `nc_close` while acquiring/releasing an optional SWMR-related file
  lock on certain filesystems, a documented issue on exactly this kind of
  environment (GitHub Actions' Windows runners; a real end user's
  network-mounted storage is the same class of risk). It only showed up
  here and not in the three plain `NetcdfFile` tests fixed just above
  because those write a small, fixed-size, uncompressed variable, while
  this is the only path in the whole test suite that closes a chunked,
  deflate-compressed variable on a genuinely *growable* (unlimited) time
  dimension - so the leading theory was HDF5's own documented file-locking
  hang on certain filesystems, fixed by setting `HDF5_USE_FILE_LOCKING=
  FALSE` centrally in `NetcdfFile::open`/`create`
  (`disableHdf5FileLockingIfUnset` in `netcdf_file.cpp`). **That theory was
  wrong** - a third CI run with the fix in place hung identically, same
  spot, same symptom. The real cause turned out to be nothing to do with
  netCDF/HDF5 internals at all: `wrftools_reproject_worker` links **both**
  Qt and GDAL/netCDF in one process, and this project already has a
  confirmed, documented fix for exactly that combination -
  `tests/fast_exit.hpp`, used by all three test binaries, for a Windows-only
  deadlock in `DllMain(DLL_PROCESS_DETACH)` teardown when a plain `return`
  from `main()` routes through `ExitProcess()`. The worker's `main()` was
  still doing a plain `return`, so it hit the identical deadlock - it had
  already finished every real GDAL warp and file write (confirmed by the
  captured progress log showing all 3 timesteps written) and simply never
  actually finished *exiting*, so `QProcess` never saw a `finished` signal
  and `ReprojectForm` waited forever for a job that had, in truth, already
  succeeded. `disableHdf5FileLockingIfUnset` was left in place (harmless,
  and HDF5's own documented workaround is still worth having regardless -
  see its own comment for why callers might still hit that class of issue
  against real network-mounted storage), but the actual fix was promoting
  `fast_exit.hpp`'s helper to a shared `include/wrftools/fast_exit.hpp`
  (`wrftools::fastExit`, `[[noreturn]]`, `TerminateProcess` on Windows) and
  calling it from every exit path in `reproject_worker.cpp`'s `main()`
  instead of `return`ing normally - safe there specifically because every
  output file is already closed and every stdout line already explicitly
  flushed by the time it's reached, so the only teardown being skipped is
  GDAL/Qt's own internal driver-unregistration bookkeeping, with no
  externally observable effect on a process about to exit anyway. The
  diagnostic 300s/420s timeouts came back down to 60s (`waitWhileRunning`)
  and 240s (`cpp.yml`'s Windows `ctest --timeout`, which also turned out to
  need genuinely more room than 120s or even 180s for the unrelated real
  LCZ-pipeline test, independent of this bug, once actually measured).

## WPS_GEOG binary dataset visualization (exceeds Python)

The Python reference only ever reads a WPS_GEOG dataset's `index` metadata
file, to resolve a display string in the Geogrid.tbl editor
(`wps_binary_index.py`) - it never renders the actual tile data (see
`README.md`'s "Known limitations"). This port adds a real View tab layer for
it:

- `wps_binary_source.hpp`/`.cpp` (`WpsBinarySource : WrfSource`) opens a
  dataset directory (an `index` file plus its numbered tile files) via
  `convert_geotiff::read_index_file`/`read_tiles` - the same tested
  index/tile-file reader the Convert tab's GeoTIFF conversion already uses
  (`convert_geotiff::GeogridIndex`/`geogrid_reader.cpp`) - and exposes it as
  a single variable (one z-level per `read()`), so it slots into the
  existing `LayerRenderer`/`ViewForm` pipeline unchanged, the same way
  `WrfFile` does for NetCDF. `isWpsGeogDataset()`/`WrfSourceRegistry::open`
  route a directory path here instead of treating it as WRF/WPS NetCDF.
  `ViewForm` gets its own "Open WPS_GEOG Dataset…" button
  (`QFileDialog::getExistingDirectory`) alongside "Open WRF/WPS NetCDF…".
  Categorical datasets (`type=categorical` in `index`) default to the
  categorical colormap with the existing 8-color/"Category N" fallback
  legend, since no named landuse/soil scheme is recorded in the index file.
- Geometry: projection from `truelat1`/`truelat2`/`stdlon` via the existing
  `Crs::lambert`/`polar`/`mercator`/`lonLat` (an arbitrary but
  self-consistent origin latitude for the projected cases, same choice
  `convert_geotiff`'s own `geotiff_writer.cpp` makes, for the same reason:
  the index file never records one). The geotransform follows WPS/GEOGRID's
  documented convention that `known_x`/`known_y` locate a grid cell's
  *center*, not a pixel corner - real-world datasets (e.g. this port's own
  `wps_soiltemp_1deg` fixture, `known_lon=-179.5`/`known_lat=-89.5` at
  `known_x=known_y=1.0`) rely on this: the true raster extent is exactly
  -180..180/-90..90, not -179.5..180.5/-89.5..90.5. A dataset's own `dy`
  sign selects whether tile-file row 1 is south (non-negative, the common
  case) or north (negative, an authoring convention observed in some
  real-world datasets - see `geotiff_writer.cpp`'s own comment); rows are
  flipped to top-down (row 0 = north) accordingly, matching `WrfFile`'s
  convention, before being handed to the shared warp/render pipeline.
  `AlbersNad83`-projected datasets (rare) are rejected with
  `UnsupportedError` - `Crs` has no Albers implementation.
- Longitude wraparound: a real-world global `regular_ll` dataset (e.g.
  GMTED2010) can record `known_lon` in a 0..360 convention rather than
  -180..180. Left alone, the portion past +180 projects to web-Mercator x
  values far outside the map's valid range instead of wrapping back to the
  western hemisphere - visually, the raster just stops partway around the
  globe (reported against a real GMTED2010 5-arc-minute dataset: Europe/
  Asia/Africa rendered, the Americas didn't). Fixed for `RegularLL` only
  (a projected CRS's meters don't wrap the same way): the west edge is
  normalized into -180..180, and a raster whose width is a full 360 degrees
  additionally gets its columns cyclic-shifted so the seam lands at
  +-180 instead of wherever `known_lon` happened to put it. A dataset that
  itself straddles the seam without being a full 360 degrees wide (rare) is
  left as-is rather than guessing how to split it.
- Unbounded warp output size (`warp.cpp`): confirmed against a real
  43200x21600 (30 arc-second) global GMTED2010 dataset -
  `topo_gmted2010_30s` from a real `WPS_GEOG` directory - opening it still
  looked broken after the longitude fix above: rendering it effectively
  hung. `GDALWarp` with no `-ts`/`-tr` override picks a target resolution
  that tries to preserve the source's native pixel size, and web Mercator's
  `1/cos(lat)` singularity at the poles inflates that further for any
  raster reaching close to +-90 latitude - together, several arc-seconds
  of native resolution plus near-pole stretching drove the natural target
  size into the gigabytes/many minutes range. `warpToWebMercator` now
  queries GDAL's own suggested target size cheaply first
  (`GDALSuggestedWarpOutput`, no resampling), and if its larger dimension
  exceeds `kMaxWarpDimension` (4096 - far more than any screen shows, and
  no smaller than what any existing WRF-domain or regional WPS_GEOG raster
  already warped to), passes an explicit `-ts` scaled down proportionally
  so `GDALWarp` resamples directly to that capped size in one pass instead
  of computing a huge one and discarding the excess. Every existing
  (much smaller) raster is unaffected - the cap only ever kicks in above
  4096, confirmed by the existing warp/render tests' unchanged output sizes.
- Tile-alignment padding (`wps_binary_source.cpp`): confirmed against a
  real `topo_gmted2010_5m` dataset - still broken after the two fixes
  above. `tile_x`/`tile_y` (600) doesn't evenly divide the true global
  size (4320x2160 at 5 arc-minutes), so `read_tiles` rounds up to a whole
  number of tiles and the dataset ships 4800x2400 columns/rows instead: the
  trailing 480 columns turned out to be a literal duplicate of columns
  0..479 (defeating the longitude-wraparound fix's `lonSpan == 360` check,
  since raw `nx*dx` came out to 400 degrees, not 360), and the trailing 240
  rows were zero-filled past the north pole (warped as if real data).
  Padding is always appended at the high-index end regardless of which
  physical direction that represents (it comes from `read_tiles` rounding
  up, not from the data itself), so `WpsBinarySource` now crops `nx`/`ny`
  down to `round(360/dx)`/`round(180/dy)` whenever the raw size exceeds
  that - before the wraparound/geotransform math runs, so a
  tile-padded-but-otherwise-normal dataset like this one still gets the
  wraparound fix on its now-correctly-sized 4320x2160.
- Wrong defaults for absent index-file keys (`geogrid_reader.cpp`), found
  by cross-checking real WPS's own `geogrid/src/source_data_module.f90`
  against `read_index_file`'s existing defaults for the same keys:
  - `tile_bdr` defaulted to 3 (this tool's own CLI default, `-b 3`) when
    the key was absent; real `geogrid.exe` defaults it to 0
    (`get_tile_dimensions`'s `if (is_tile_bdr(idx)) ... else npts_bdr = 0`).
    The wrong default doesn't just misplace pixels - it makes
    `read_tiles` expect a much larger tile file than a real 0-border
    dataset actually has, so it would have failed outright with "Tile
    file is shorter than expected" on any real dataset that omits this key
    (none of the three tested against today happened to).
  - `missing_value` defaulted to 0 when absent, silently masking any
    real value of exactly 0 (e.g. sea-level elevation, common over ocean)
    as missing/transparent. Real `geogrid.exe` treats an absent
    `missing_value` as "no value is missing at all"
    (`process_tile_module.f90`: `if (istatus /= 0) msg_val = NAN`). Now
    defaults to NaN, which conveniently also means "never equal, never
    masks anything" for the existing `value == idx.missing` check with no
    extra branching needed. Affects both real GMTED2010 datasets tested
    (both omit `missing_value`) - previously any true sea-level (0m) pixel
    rendered as a transparent gap rather than teal-colored 0m terrain.
  - `row_order` was never actually read from the index file at all -
    `read_index_file` hardcoded `bottom_top` unconditionally, and
    `WpsBinarySource`/`geotiff_writer.cpp` independently (mis)inferred
    row order from `dy`'s sign instead. Real `geogrid.exe` reads an
    explicit `row_order` key (defaulting to `bottom_top`, matching this
    port's now-corrected default) via its own `get_row_order`, entirely
    unrelated to `dy`'s sign. Fixed to parse the real key and use
    `idx.bottom_top` (not `dy`'s sign) in both consumers. No real dataset
    tested against today sets `row_order = top_bottom`, so this was a
    latent bug rather than an observed one - covered by a hand-built
    regression test instead.
  All three read the same tested real datasets identically before and
  after (each one happens to set `tile_bdr` explicitly, and none sets
  `row_order`), confirmed by re-running the geometry probe against both
  GMTED2010 datasets post-fix.
- `convert_geotiff_lib` was split in `CMakeLists.txt`: the index/tile
  reader (`geogrid_index.cpp`, `geogrid_reader.cpp` - no TIFF/GEOTIFF
  dependency) now lives in its own `convert_geotiff_reader` target, linked
  by `wrftools_core`; the actual GeoTIFF read/write code
  (`geotiff_reader.cpp`/`geotiff_writer.cpp`/`convert.cpp`/`convert_back.cpp`,
  which do need libtiff/libgeotiff) stays in `convert_geotiff_lib`, linked
  only by `wrftools_ui` for the Convert tab - `wrftools_core` still never
  depends on TIFF/GEOTIFF.
- Not addressed here: `convert_geotiff`'s own GeoTIFF <-> geogrid conversion
  (`geotiff_reader.cpp`/`geotiff_writer.cpp`) always treats `known_x`/
  `known_y` as a pixel *corner*, not the cell-center convention above -
  self-consistent for its own round-trip (write then read back the same
  tool's own output) but not necessarily geodetically accurate against a
  real-world dataset's `index` file on the way in or out. Out of scope here
  since it's a pre-existing property of the vendored Convert tab, not the
  new View tab visualization path.

## Non-goals retained from Python

- No checked support for antimeridian-crossing or polar display domains.
- No rotated lat/lon WRF/WPS projection support.
- Single-file internal times remain index-labelled when GDAL cannot expose
  `Times`; filename-based multi-file series can expose real labels.
