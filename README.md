# WRF Tools

A standalone (no QGIS required) WRF/WPS tool with two tabs on one shared
map: **Domains**, for defining nested model domains and importing/exporting
`namelist.wps` (extracted from the
[GIS4WRF](https://github.com/GIS4WRF/gis4wrf) QGIS plugin's domain wizard);
and **View**, for opening WRF/WPS NetCDF files (`geo_em*`, `met_em*`,
`wrfinput*`, `wrfout*` - including a multi-select of several same-domain
files from a split-per-timestep run, auto-combined into one file with a
single time axis, see `wrfseries.py`) and drawing a configurable stack of
colored raster layers - one per (variable, time step, vertical level,
colormap, unit, opacity, interpolate-on-display) selection - over the same
basemap, so
terrain/land-use/meteorology can be checked against where the domains were
actually configured. A colorbar for the selected layer (with configurable
tick count/number format) is drawn directly on the map
(`wrftools/colorbar.py`) as a movable overlay - drag it anywhere and it
stays put across redraws; a known-categorical variable (`LU_INDEX`,
`IVGTYP`, soil-type fields, ...) auto-selects a discrete swatch-per-class
legend instead of a gradient, using the file's own WRF `LANDUSE.TBL` colors
where available. An optional, independently movable info overlay ("Show
Info Overlay" in the Colorbar box) shows the selected layer's variable,
unit, and current time as one label. An optional, also-movable north arrow
("Show North Arrow" in the View box) always points straight up, since the
map itself never rotates. For a multi-timestep layer (typically a
`wrfseries.WRFFileSeries`), a Play button next to the time step steps
through every timestep automatically, looping back to the start at the end.
The whole map view - basemap, layers, outlines, legend - can be exported as
a PNG/JPEG via `File > Export Map Image...`.

Deliberately lightweight: no QGIS, no Chromium/QWebEngine, no third-party
map/web library, and no `netCDF4` dependency for reading WRF files - GDAL's
own netCDF driver is sufficient (`wrftools/wrfreader.py`). The map is a
small hand-rolled XYZ tile widget (`wrftools/tilemap.py`) built on PyQt6
alone, with support for both vector overlays (domain outlines) and raster
overlays (View-tab layers) drawn in independent, z-ordered groups so the two
tabs can update their own overlays without disturbing each other's. Domain
geometry, CRS handling, and namelist I/O reuse GDAL/OGR/OSR directly
(`gis4wrf.core`, vendored - see below); colormaps (`wrftools/colormaps.py`,
including a categorical mode and a `jet` map) are small numpy-only LUTs, no
matplotlib; unit conversion (`wrftools/units.py`, e.g. K -> degC, m/s ->
knots) is a small explicit table, not a general unit-parsing library.

## Setup

Linux and macOS are both supported (developed on Linux; regularly run and
packaged on macOS too - see [Packaging](#packaging) below). Install GDAL
first - `gdal-devel`/`libgdal-dev` on Linux, or `brew install gdal` on
macOS - then:

```
./setup.sh
```

`pyproject.toml`'s `gdal` pin has to match this machine's system `libgdal`
*exactly* - GDAL's Python bindings enforce that at build time, and there's
no version range that works across machines with different `libgdal`
versions installed. `setup.sh` detects the local version (via
`gdal-config`) and pins to it before syncing, so this works out of the box
regardless of what `libgdal` version is on your system. Re-run it after
pulling if you see a GDAL build error like:

```
Exception: Python bindings of GDAL X require at least libgdal X, but Y was found
```

That means the committed pin (set by whoever last ran `setup.sh` on their
own machine) doesn't match yours - it's local, per-machine config, not a
real dependency change, so don't worry about "reverting" someone else's
pin when you fix yours.

On a machine that also has Anaconda/Miniconda installed, make sure a
`gdal-config` from Homebrew (or your Linux distro's package) is the one
that resolves first on `PATH` - conda ships its own, often much older,
`gdal-config`, and `setup.sh` will happily (but wrongly) pin to whichever
one it finds first. `setup.sh` prints a note if it detects this.

## Run

```
uv run wrftools
```

## Packaging

```
./build.sh
```

Produces a standalone single-file executable at `dist/wrftools` on
Linux, or an app bundle at `dist/wrftools.app` on macOS (launch with
`open dist/wrftools.app`, or run the executable inside it directly -
both work correctly). See the comments in `build.sh` for why it needs a
few things PyInstaller's automatic analysis misses (numpy's BLAS backend
on some Linux distros, GDAL/PROJ data files, a numpy submodule GDAL's C
extension imports in a way static analysis can't see) and adjust the
paths there if your system layout differs. On macOS the built app is
ad-hoc signed by PyInstaller; Gatekeeper may still require right-click ->
Open the first time, or `xattr -dr com.apple.quarantine dist/wrftools.app`
if it was downloaded/copied from another machine.

**On macOS, this needs `--onedir --windowed`, not `--onefile`** (which
`build.sh` selects automatically there - see the mode-selection comment
in `build.sh` for the reasoning in full). Getting either half wrong
produces a binary that *looks* fine (builds cleanly, launches with no
error) but never actually shows a window:

- Without `--windowed`, the build has no Info.plist/app-bundle identity,
  so macOS never grants it real foreground-application status - Qt's cocoa
  platform plugin loads fine and the event loop runs, but no window ever
  becomes visible and nothing is printed, since nothing crashed.
- `--onefile` on macOS is worse than just unnecessary once `--windowed` is
  in the mix: a onefile build re-extracts its entire payload (every
  bundled GDAL/Qt/numpy `.so`/`.dylib` - several hundred files) to a fresh
  temp directory on *every launch*, and macOS's ad-hoc-signature
  validation can't cache "already validated" across launches for files at
  a new path each time (unlike a real, once-installed `.app`, which it
  does cache). Confirmed by hand: sampling the running-but-windowless
  process showed it stuck almost entirely in dyld's signature-validation
  path minutes after launch, until macOS killed it outright for taking too
  long to present a window (a managed termination, not a crash - so still
  no error anywhere). `--onedir` extracts once, at build time, into a
  stable bundle layout, which avoids this entirely - PyInstaller itself
  flags onefile+windowed on macOS as deprecated specifically because of
  this clash with macOS's security model.

**On Linux, running `build.sh` directly only produces a binary that works
on machines with glibc >= the build machine's** (this doesn't apply to
macOS, which has no glibc). PyInstaller bundles shared libraries (Qt,
glib, GDAL, ...) as found on the build machine, and those carry that
machine's glibc symbol-version requirements baked in - a binary built on
a bleeding-edge system (this project's normal dev machine runs glibc
2.43) fails on anything older with an error like:

```
ImportError: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.43' not found
```

For a Linux binary that runs on essentially any current Linux system
(including when building from macOS, via Docker), build inside the
provided Docker image instead, which uses Debian 12 (glibc 2.36)
specifically because it's old enough to cover the overwhelming majority
of currently-supported distros while still having a modern enough GDAL to
build cleanly (see the comments in `Dockerfile.build` for why plain
manylinux_2_28, the more conventional choice for this kind of thing,
doesn't work here):

```
./build-portable.sh
```

This always produces a Linux binary (it builds inside a Linux container
regardless of host OS), so it's not useful for a portable *macOS* binary -
macOS has no glibc-style forward-compatibility problem to work around in
the first place, since Apple doesn't support running a binary built on a
newer macOS on an older one anyway; just use `build.sh` there.

The target Linux machine still needs the base graphics stack essentially
every Linux desktop already has (`libgl1`/`libegl1` or equivalent) - GL/EGL
libraries are deliberately *not* bundled, since they're tied to the actual
GPU driver in use; a bundled generic copy would be wrong on the target
machine, not just redundant.

## Project layout

- `src/wrftools/tilemap.py` - the map widget: tile math, HTTP fetch +
  disk cache via `QtNetwork`, mouse pan/zoom, lon/lat overlay drawing, and
  `export_image()` (a plain `grab().save(path)` of the current view, wired to
  `app.py`'s `File > Export Map Image...`) - whatever's currently on the
  widget (basemap, every overlay group, the legend), not a re-render at a
  different resolution.
- `src/wrftools/domainform.py` - the domain wizard form, ported from
  GIS4WRF's `widget_domains.py`. No QGIS `iface` dependency: reads the map's
  current view extent directly, and "Set from File" reads a raster/vector
  file's extent via GDAL/OGR instead of relying on a QGIS layer list.
- `src/wrftools/domainoverlay.py` - turns a `gis4wrf.core.Project`'s
  domains into lon/lat polygons for the map widget (reprojects and densifies
  via GDAL/OSR so curved projections like Lambert Conformal render
  accurately, not as straight-edged boxes).
- `src/wrftools/fileextent.py` - GDAL/OGR-based file extent reader.
- `src/wrftools/formhelpers.py` - validated line-edit widgets, ported
  from GIS4WRF's `plugin/ui/helpers.py`.
- `src/wrftools/wrfreader.py` - opens WRF/WPS NetCDF files via GDAL
  (no `netCDF4`) and exposes their variables/time steps/vertical levels,
  CRS, and geotransform. Not a port of GIS4WRF's (unvendored)
  `wrf_netcdf_to_gdal.py` - reimplemented against GDAL's own netCDF driver;
  see the module docstring for the several real orientation/coordinate
  pitfalls this involved. Also flags known categorical variables
  (`WRFVariable.category_scheme`: `LU_INDEX`/`IVGTYP` resolved against the
  file's own `MMINLU` landuse scheme, soil-type fields and any
  `units == 'category'` variable flagged with an unnamed scheme) for
  `rasterlayer.py`'s categorical rendering path.
- `src/wrftools/wrfseries.py` - groups several same-domain WRF/WPS files
  (the common `frames_per_outfile=1` convention - one output file per
  timestep, e.g. `wrfout_d03_2025-03-14_00_00_00`, `..._00_30_00`, ...) into
  a `WRFFileSeries` with one time axis spanning all of them, purely from
  recognizing the filename pattern (`wrfout`/`wrfrst`/`met_em`, domain
  number, valid time) - no changes needed anywhere else, since
  `WRFFileSeries` exposes the exact same interface as a single `WRFFile`
  (`.read()`/`.crs`/`.geotransform`/`.variables`/`.times`/`.name`, ...).
  Lazy: opening a series only opens its first file (needed immediately for
  its grid/variables), not the rest - each other file opens on first read of
  one of its own timesteps; see Known limitations for what that trades off.
  Since each file's own name carries its exact valid time, a series shows
  real timestamps in the Time dropdown instead of `wrfreader.py`'s
  placeholder `Step N of M` labels.
- `src/wrftools/colormaps.py` - small numpy-only named color LUTs
  (viridis, plasma, magma, cividis, coolwarm, terrain, greys, jet) plus a
  categorical LUT (`colormaps.CATEGORICAL`) for landuse-style variables,
  reusing `gis4wrf.core.readers.categories`. GIS4WRF has no per-layer
  colormap choice at all (continuous variables render as plain greyscale),
  so this is new, not ported.
- `src/wrftools/units.py` - a small explicit unit-conversion table
  (K -> degC/degF, m/s -> km/h/knots/mph, Pa -> hPa/inHg, m -> ft/km, and a
  couple of precipitation units) for the View tab's per-layer unit picker.
  Not a general unit-parsing library - WRF/WPS output uses a small, fixed
  set of unit strings.
- `src/wrftools/rasterlayer.py` - the View tab's layer model
  (`RasterLayer`) and its three-tier render/cache pipeline
  (`LayerRenderer`): open file handles, a byte-bounded cache of warped
  (EPSG:3857) arrays, and a count-bounded cache of colormapped images.
  `RasterLayer.interpolate`, and its colorbar tick count/format/decimals,
  control display only (`RasterOverlay.smooth`,
  `QPainter.RenderHint.SmoothPixmapTransform`, and the legend respectively)
  - paint-time only, like opacity/visibility, so none of them invalidate a
  cache entry. `RasterLayer.units`, unlike those, *is* part of the image
  cache key, since it changes the rendered pixels.
- `src/wrftools/colorbar.py` - builds the View tab's on-map colorbar
  legend as a QPixmap: `build_legend_pixmap()` draws a gradient with a
  configurable number of evenly spaced ticks (`RasterLayer.tick_count`) in
  a configurable format (`RasterLayer.tick_format`/`tick_decimals`: auto/
  fixed/scientific) for the selected layer's colormap and effective range
  (`LayerRenderer.effective_range`); `build_categorical_legend_pixmap()`
  draws a color-swatch-per-class list instead, for a categorical layer
  (`LayerRenderer.categorical_legend`). Both are drawn via
  `TileMapWidget.set_legend()` as a *movable* overlay - defaulting to the
  map's top-right corner, but draggable anywhere with the mouse (its
  position persists across redraws until dragged again) - independent of
  the geo-referenced overlay groups.
- `src/wrftools/viewform.py` - the View tab: open files (multi-select; files
  sharing a recognized WRF domain+kind naming pattern auto-combine into one
  `wrfseries.WRFFileSeries` entry, everything else opens individually as
  before), add/remove/reorder layers, and configure each layer's
  variable/time/level/colormap/units/opacity/range/interpolate/colorbar-ticks.
  A categorical variable auto-selects the categorical colormap (still
  manually overridable via the same dropdown); the unit picker is hidden
  entirely for a variable with no known conversions. A Play button next to
  the time step control (enabled whenever the selected layer's file has more
  than one timestep) steps through every timestep on a timer, looping back
  to the start at the end - handy for a multi-file series. "Show Info
  Overlay" in the Colorbar box adds a second, independently movable overlay
  (`TileMapWidget.set_info_text()`) showing the selected layer's variable,
  unit, and current time as one label, kept in sync as either changes.
- `src/gis4wrf/core/` - **vendored**, see below.

## Vendored `gis4wrf.core`

`src/gis4wrf/core/` is a **trimmed subset** of the `gis4wrf/core` subpackage
from the [GIS4WRF repo](https://github.com/GIS4WRF/gis4wrf) (domain/CRS
math, namelist read/write, GDAL outline generation - all QGIS-independent
code). It's vendored rather than depended on because the GIS4WRF repo isn't
a pip-installable package (it's zipped directly for QGIS's plugin loader,
see its `build.py`), so there's nothing for `uv add` to point at.

Only the files this app's actual import graph reaches are kept - not the
full `gis4wrf/core` tree. `src/gis4wrf/core/__init__.py` here only imports
the modules this app needs (`errors`, `crs`, `project`, `readers.namelist`,
`writers.namelist`, and the three `transforms.*` conversion functions),
unlike the real one which unconditionally imports everything in `core/`
(downloaders, WRF/WPS-binary-to-GDAL conversion, grib/shapefile readers -
none of which the domain wizard touches). What's dropped: `program.py`,
`downloaders/{dist,geo,met,plugin_version,util}.py`,
`readers/{grib_metadata,wrf_netcdf_metadata}.py`,
`writers/{shapefile,wps_binary}.py`,
`transforms/{categories_to_gdal,project_to_gdal_checkerboards,project_to_wrf_namelist,wps_binary_to_gdal,wrf_netcdf_to_gdal}.py`.
Both `nml_schemas/{wps,wrf}.{json,yml}` are kept even though only the `wps`
schema is used directly, since the vendored `Project` class still has a
(here-unused) method that reads the `wrf` one, and shipping two small text
files is cheaper than leaving that method silently broken.

`src/gis4wrf/__init__.py` here is **not** a copy of the real
`gis4wrf/__init__.py` - that one wires up QGIS plugin bootstrapping
(`classFactory`, `bootstrap_with_ui`) which needs `qgis` and pulls in
`pkg_resources`/`setuptools` for its own dependency installer. None of that
applies here, so this package's `__init__.py` is just a docstring.

To pull in upstream changes, re-sync the `core` subtree and re-trim, e.g.:

```
rsync -a --exclude='__pycache__' /path/to/gis4wrf/gis4wrf/core/ src/gis4wrf/core/
git checkout src/gis4wrf/core/__init__.py   # rsync overwrites it with the untrimmed original
```

then re-apply whatever trimming still makes sense against the new upstream
version (importing `gis4wrf.core` and checking `sys.modules` for
`gis4wrf.*` after the fact confirms exactly what's actually reachable, the
same way this trim was derived).

**One deliberate local deviation from upstream that a re-sync would silently
revert:** `crs.py`'s `CRS.WRF_DATUM_PROJ4` is `WRF_PROJ4_SPHERE` (the WRF earth
radius, 6370000 m) here, not upstream's `'+datum=WGS84'`. Upstream's own code
carried a `#FIXME` acknowledging that was wrong. Measured on this repo's test
fixtures: the WGS84 datum made WRF's (square-by-construction) grid cells
0.3-0.6% anisotropic, and shifted a domain's displayed lon/lat placement by
~1 km on a several-hundred-km domain, because WRF's own grid spacing is
computed with spherical projection formulas - using the WGS84 ellipsoid
instead doesn't reproduce where WRF actually places a domain. See
`tests/test_crs_datum.py` for the regression coverage. Re-apply this override
after any re-sync.

## Known limitations

- Tile math and lon/lat projection is spherical-Mercator-for-display only;
  it hasn't been checked against antimeridian-crossing or polar domains.
- No checkerboard/grid-cell overlay (the QGIS plugin renders one via a GDAL
  VRT + pixel function built for QGIS's raster layer pipeline); domain
  *outlines* are rendered, which is what matters for checking nest
  placement.
- Basemap uses OpenStreetMap's standard tile server (the GIS4WRF plugin's
  Stamen-based basemap is dead - Stamen's tile servers were shut down).
- Domains are edited as a tree (a `QTreeWidget`, not a fixed chain), so two
  domains sharing a parent (siblings) are fully supported - both for
  namelist import/export and for building them directly in the UI (select a
  domain, "Add Child Domain" twice for two siblings). See
  `PLAN_TREE_DOMAINS.md`.
- View tab: time steps within a single file are labelled by index ("Step N
  of M"), not by their real timestamp - the `Times` char variable isn't
  readable through either of GDAL's netCDF APIs on the files this was
  checked against (see `wrfreader.py`'s docstring). A multi-file series
  (`wrfseries.py`) opened via multi-select *does* show real timestamps,
  since each file's own name carries its exact valid time - unless any file
  in the series has more than one internal timestep itself, in which case it
  falls back to the same step-index labeling. Only NetCDF WRF/WPS files are
  supported (no WPS *binary* geographical datasets - GIS4WRF's separate
  `wps_binary_to_gdal.py` path, not ported). `U`/`V` are destaggered by a
  simple adjacent-cell average.
- Multi-file series (`wrfseries.py`) open lazily: only the first file is
  opened when the series itself is opened (~1.3s for a real 12-file/
  164-variable series, down from ~17s opening all 12 up front), and every
  other file opens - and is checked against the first file's grid - only the
  first time one of its own timesteps is actually read, so a mismatch deep
  in a long series surfaces when the user steps to that time, not when the
  series is opened. The one case that still opens every file up front is a
  series whose files themselves contain more than one internal timestep
  each (`frames_per_outfile > 1`) - a file's name alone can't say how many
  timesteps it holds. Grouping is filename-based only (domain number + a
  recognized naming pattern); it can't tell two different runs apart if
  their output happens to share a directory and domain number and produces
  the same grid by coincidence.
