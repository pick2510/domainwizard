# WRF Tools

A standalone (no QGIS required) native C++20/Qt6/GDAL WRF/WPS tool with
three tabs on one shared map: **Domains**, for defining nested model
domains (dragged and resized directly on the map, or edited numerically)
and importing/exporting `namelist.wps`; **View**, for opening WRF/WPS
NetCDF files (`geo_em*`, `met_em*`, `wrfinput*`, `wrfout*` - including a
multi-select of several same-domain files from a split-per-timestep run,
auto-combined into one file with a single time axis) *and* raw WPS_GEOG
binary static-data directories (e.g. `topo_gmted2010_30s`), drawing a
configurable stack of colored raster layers - one per (variable, time
step, vertical level, colormap, unit, opacity, interpolate-on-display)
selection - over the same basemap, so terrain/land-use/meteorology can be
checked against where the domains were actually configured; and
**Convert**, for GeoTIFF <-> WPS geogrid binary conversion. A colorbar for
the selected layer (with configurable tick count/number format) is drawn
directly on the map as a movable, resizable overlay; a known-categorical
variable (`LU_INDEX`, `IVGTYP`, soil-type fields, ...) auto-selects a
discrete swatch-per-class legend instead of a gradient, using the file's
own WRF `LANDUSE.TBL`/`MODIFIED_IGBP_MODIS_NOAH` colors where available. An
optional, independently movable info overlay ("Show Info Overlay" in the
Colorbar box) shows the selected layer's variable, unit, and current time
as one label. For a multi-timestep layer, Previous/Next buttons step
through timesteps one at a time (wrapping at either end), a Play button
loops through them automatically at a configurable interval, and the info
overlay's timestamp updates alongside. The whole map view - basemap,
layers, outlines, legend - can
be exported as a PNG/JPEG via `File > Export Map Image...`. The app
follows the OS's light/dark theme automatically, with a manual
`Options > Theme` override.

Deliberately lightweight: no QGIS, no Chromium/QWebEngine, no third-party
map/web library. The map is a small hand-rolled XYZ tile widget
(`tile_map_widget.hpp`/`.cpp`) built on Qt Widgets alone, with support for
both vector overlays (domain outlines) and raster overlays (View-tab
layers) drawn in independent, z-ordered groups so the Domains and View
tabs can update their own overlays without disturbing each other's. Domain
geometry, CRS handling, and namelist I/O are built directly on GDAL/OGR/OSR
(`crs.hpp`/`.cpp`, `domain.hpp`/`.cpp`, `wps_namelist.hpp`/`.cpp`);
colormaps (`colormaps.hpp`/`.cpp`, including a categorical mode and a
`jet` map) are small fixed-stop LUTs; unit conversion (`units.hpp`/`.cpp`,
e.g. K -> degC, m/s -> knots, Pa -> hPa, m -> ft, mm -> in) is a small
explicit table, not a general unit-parsing library.

## Setup

Linux and macOS are both supported (developed on Linux; built and tested
on macOS too, via CI - see [Packaging](#packaging) below).

On Debian/Ubuntu install the native development dependencies (`libtiff`/
`libgeotiff` come in as transitive dependencies of `libgdal-dev`, so
they're not listed explicitly):

```
sudo apt install cmake ninja-build g++ qt6-base-dev libgdal-dev libproj-dev catch2
```

On macOS with Homebrew:

```
brew install cmake ninja qt gdal proj catch2
```

## Run

```
cmake -S . -B build-cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug
# On macOS, also pass -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build-cpp
ctest --test-dir build-cpp --output-on-failure
./build-cpp/wrftools
```

## Packaging

Every push/PR builds, tests, and packages the app on both Linux and macOS
via GitHub Actions (`.github/workflows/cpp.yml`), and uploads the portable
`wrftools-linux`/`wrftools-macos` bundles as downloadable workflow
artifacts - no local build needed just to try it.

To build a portable bundle locally instead:

```
./build-portable-cpp.sh    # Linux -> dist/wrftools-cpp/
./build-portable-macos.sh  # macOS -> dist/wrftools.app, dist/wrftools-macos.zip
```

`build-portable-cpp.sh` (`cmake/bundle_linux.cmake`) builds Release and
bundles the executable, its full shared-library closure
(`file(GET_RUNTIME_DEPENDENCIES)`, including the Qt `platforms`/`tls`/
`imageformats` plugins resolved via a second pass since they're dlopen'd),
and GDAL/PROJ's data files, then `patchelf --set-rpath`s everything to
`$ORIGIN`-relative paths - self-contained aside from libc/libstdc++/the
dynamic loader, which stay tied to a comparably-recent-or-newer glibc than
the build machine's. Requires `patchelf` (`apt install patchelf`).

`build-portable-macos.sh` builds Release and bundles the app via Qt's own
`macdeployqt` (Qt frameworks/plugins) followed by `dylibbundler`
(everything else non-system: GDAL, PROJ, and their own transitive deps),
then copies GDAL/PROJ's data directories into `Contents/share/{gdal,proj}`
beside the executable so no `GDAL_DATA`/`PROJ_DATA` environment setup is
needed at run time. Requires `dylibbundler` (`brew install dylibbundler`).

The target Linux machine still needs the base graphics stack essentially
every Linux desktop already has (`libgl1`/`libegl1` or equivalent) - GL/EGL
libraries are deliberately *not* bundled, since they're tied to the actual
GPU driver in use; a bundled generic copy would be wrong on the target
machine, not just redundant.

## Project layout

- `src/main.cpp` - entry point: GDAL data-directory discovery (so a
  portable bundle finds its own bundled `GDAL_DATA`/`PROJ_DATA` instead of
  a system install) and `MainWindow` construction.
- `src/main_window.cpp` - the top-level window: the Domains/View/
  Convert tab set sharing one `TileMapWidget`, `File > Export Map Image`,
  and the `Options > Theme` (System/Light/Dark) menu.
- `src/tile_map_widget.cpp` - the map widget: Web Mercator tile math,
  async tile fetch with an on-disk cache, mouse pan/zoom, named z-ordered
  raster/vector overlay groups, drag-to-move/resize handling for both
  domain outlines and the colorbar/info overlays, and `exportImage()`.
- `src/domain_form.cpp` - the Domains tab: an editable domain tree
  (siblings supported, not just a linear chain), namelist import/export,
  and wiring domain edits (including on-map drag/resize) into
  `domain_overlay.cpp`'s outlines.
- `src/domain_overlay.cpp` - turns a domain tree's bboxes into
  densified, projected-then-reprojected-to-lon/lat outline polygons for
  the map widget, so curved projections like Lambert Conformal render
  accurately rather than as straight-edged boxes.
- `src/domain.cpp` - domain bbox/padding/containment math (Lambert,
  Polar, Mercator, and lon/lat projections) shared by the Domains tab and
  its tests.
- `src/crs.cpp` - builds a WRF/WPS coordinate reference system (on
  WRF's own spherical earth radius, not WGS84) from proj4 strings, with
  cached coordinate transforms since a naive per-call
  `OGRCreateCoordinateTransformation` was measurably slow across a whole
  domain outline's worth of vertices.
- `src/wps_namelist.cpp` - `namelist.wps` read/write, preserving
  unknown groups/keys (e.g. `&metgrid`, `&ungrib`, `geog_data_*`) on
  export rather than dropping them.
- `src/view_form.cpp` - the View tab: open files/series or a
  WPS_GEOG binary dataset directory, add/remove/reorder layers, and
  configure each layer's variable/time/level/colormap/units/opacity/
  range/interpolate/colorbar-ticks. A categorical variable auto-selects
  the categorical colormap; playback steps through every timestep on a
  timer.
- `src/wrf_file.cpp` - opens a WRF/WPS NetCDF file via GDAL's own
  netCDF driver (no separate NetCDF library dependency), exposing its
  variables, geotransform (derived from the staggered U/V coordinate
  grids, not the mass grid), and CRS; destaggers `U`/`V` by simple
  adjacent-cell averaging.
- `src/wrf_series.cpp` - groups several same-domain WRF/WPS files
  (the common `frames_per_outfile=1` convention) into one time axis
  spanning all of them, purely from a recognized filename pattern; lazy -
  only the first file is opened up front, the rest open on first read of
  one of their own timesteps.
- `src/wrf_source.cpp` / `src/wps_binary_source.cpp` - the
  `WrfSource` interface `LayerRenderer` renders against (`WrfFile`,
  `WrfFileSeries`, or `WpsBinarySource` behind one uniform surface).
  `wps_binary_source.cpp` reads a raw WPS_GEOG binary dataset directory
  (an `index` file plus numbered tile files - the format `geogrid.exe`
  itself reads/writes) directly, including real-world edge cases like
  0..360 longitude datasets, tile-alignment padding beyond the true
  global extent, and an explicit `row_order` key - something the
  original Python implementation never visualized at all.
- `src/layer_renderer.cpp` / `src/raster_layer.cpp` /
  `src/warp.cpp` - the View tab's layer model and its two-tier
  render/cache pipeline: an open-file registry, a byte-bounded cache of
  warped (EPSG:3857) arrays (`warp.cpp`, via an in-memory `GDALWarp`,
  output-size-capped so a very high-resolution global raster warps in a
  bounded amount of time near the poles instead of unboundedly), and a
  count-bounded cache of colormapped images.
- `src/colormaps.cpp` - fixed-stop named color LUTs (viridis, plasma,
  magma, cividis, coolwarm, terrain, greys, jet) plus a categorical LUT
  (real WRF landuse/soil-type class colors where the scheme is known,
  falling back to a deterministic 8-color cycle otherwise).
- `src/units.cpp` - a small explicit unit-conversion table (K ->
  degC/degF, m/s -> km/h/knots/mph, Pa -> hPa/inHg, m -> ft/km, mm -> in)
  for the View tab's per-layer unit picker.
- `src/colorbar.cpp` - builds the View tab's on-map colorbar legend:
  a gradient with a configurable number of evenly spaced ticks (auto/
  fixed/scientific format) for a continuous layer, or a color-swatch-per-
  class list for a categorical one.
- `src/theme.cpp` - OS light/dark theme detection (Qt's own
  `QStyleHints::colorScheme()` where available, with a GNOME `gsettings`
  fallback for desktops where that's unreliable) and application, plus
  the persisted manual `System`/`Light`/`Dark` override.
- `src/geotiff_convert_form.cpp` - the Convert tab: a Qt GUI over the
  vendored `convert_geotiff` library (see below).

## Vendored `convert_geotiff`

`src/convert_geotiff/`/`include/convert_geotiff/` is
[convert_geotiff](https://github.com/jbeezley/convert_geotiff) (public
domain, by jbeezley), a small, GDAL-free GeoTIFF <-> WPS geogrid binary
conversion library, vendored unmodified except for its GUI - the original
FLTK interface was replaced with `GeotiffConvertForm`, marshaling its
background conversion thread's progress/completion back to the Qt GUI
thread via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` instead
of FLTK's `Fl::lock()`/`Fl::awake()`.

The index-file/tile-data reader (`geogrid_index.cpp`, `geogrid_reader.cpp`
- no TIFF/GEOTIFF dependency) is built as its own `convert_geotiff_reader`
CMake target so `wrftools_core`'s `WpsBinarySource` (View tab
visualization) can reuse it without pulling libtiff/libgeotiff into the
core library; the GeoTIFF read/write code, which does need those, stays in
`convert_geotiff_lib`, linked only by the GUI for the Convert tab.

## Known limitations

- No checked support for antimeridian-crossing or polar *display* domains
  (a WPS_GEOG raster layer's own global-dataset antimeridian wraparound
  is handled - see `wps_binary_source.cpp`).
- No rotated lat/lon WRF/WPS projection support.
- No `albers_nad83` WPS_GEOG source projection support (rare in practice;
  fails with a clear error rather than misreading).
- Single-file internal times remain index-labelled ("Step N of M") when
  GDAL cannot expose a file's own `Times` variable; a filename-based
  multi-file series exposes real timestamps instead, unless any file in
  it has more than one internal timestep itself.
- No checkerboard/grid-cell overlay; domain *outlines* are rendered, which
  is what matters for checking nest placement.
- Basemap uses OpenStreetMap's standard tile server.

This app was ported from an earlier Python/PyQt6 implementation, since
removed from the repository; see `PORT.md` for the full porting history
(the Python source itself is still recoverable from git history before
this removal, if ever needed).
