# Domain Wizard

A standalone (no QGIS required) WRF/WPS domain configuration tool: define
nested model domains on an interactive map and import/export
`namelist.wps`. Extracted from the [GIS4WRF](https://github.com/GIS4WRF/gis4wrf)
QGIS plugin's domain wizard.

Deliberately lightweight: no QGIS, no Chromium/QWebEngine, no third-party
map/web library. The map is a small hand-rolled XYZ tile widget
(`domainwizard/tilemap.py`) built on PyQt6 alone; domain geometry, CRS
handling, and namelist I/O reuse GDAL/OGR/OSR directly (`gis4wrf.core`,
vendored - see below).

## Setup

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

## Run

```
uv run domainwizard
```

## Packaging

```
./build.sh
```

Produces a standalone single-file executable at `dist/domainwizard`. See
the comments in `build.sh` for why it needs a few things PyInstaller's
automatic analysis misses (numpy's BLAS backend, GDAL/PROJ data files,
a numpy submodule GDAL's C extension imports in a way static analysis
can't see) and adjust the paths there if your system layout differs.

**Running `build.sh` directly only produces a binary that works on
machines with glibc >= the build machine's.** PyInstaller bundles shared
libraries (Qt, glib, GDAL, ...) as found on the build machine, and those
carry that machine's glibc symbol-version requirements baked in - a binary
built on a bleeding-edge system (this project's normal dev machine runs
glibc 2.43) fails on anything older with an error like:

```
ImportError: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.43' not found
```

For a binary that runs on essentially any current Linux system, build
inside the provided Docker image instead, which uses Debian 12 (glibc
2.36) specifically because it's old enough to cover the overwhelming
majority of currently-supported distros while still having a modern
enough GDAL to build cleanly (see the comments in `Dockerfile.build` for
why plain manylinux_2_28, the more conventional choice for this kind of
thing, doesn't work here):

```
./build-portable.sh
```

The target machine still needs the base graphics stack essentially every
Linux desktop already has (`libgl1`/`libegl1` or equivalent) - GL/EGL
libraries are deliberately *not* bundled, since they're tied to the actual
GPU driver in use; a bundled generic copy would be wrong on the target
machine, not just redundant.

## Project layout

- `src/domainwizard/tilemap.py` - the map widget: tile math, HTTP fetch +
  disk cache via `QtNetwork`, mouse pan/zoom, lon/lat overlay drawing.
- `src/domainwizard/domainform.py` - the domain wizard form, ported from
  GIS4WRF's `widget_domains.py`. No QGIS `iface` dependency: reads the map's
  current view extent directly, and "Set from File" reads a raster/vector
  file's extent via GDAL/OGR instead of relying on a QGIS layer list.
- `src/domainwizard/domainoverlay.py` - turns a `gis4wrf.core.Project`'s
  domains into lon/lat polygons for the map widget (reprojects and densifies
  via GDAL/OSR so curved projections like Lambert Conformal render
  accurately, not as straight-edged boxes).
- `src/domainwizard/fileextent.py` - GDAL/OGR-based file extent reader.
- `src/domainwizard/formhelpers.py` - validated line-edit widgets, ported
  from GIS4WRF's `plugin/ui/helpers.py`.
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

## Known limitations

- Tile math and lon/lat projection is spherical-Mercator-for-display only;
  it hasn't been checked against antimeridian-crossing or polar domains.
- No checkerboard/grid-cell overlay (the QGIS plugin renders one via a GDAL
  VRT + pixel function built for QGIS's raster layer pipeline); domain
  *outlines* are rendered, which is what matters for checking nest
  placement.
- Basemap uses OpenStreetMap's standard tile server (the GIS4WRF plugin's
  Stamen-based basemap is dead - Stamen's tile servers were shut down).
- Namelist import/export supports arbitrary domain trees, including two
  domains sharing a parent (siblings) - see `PLAN_TREE_DOMAINS.md`. The
  interactive UI (the domain wizard form itself) doesn't yet: it can only
  display/edit a single linear chain, and gives a clear error rather than
  silently doing the wrong thing if you import a namelist with siblings.
  Building the tree-editing UI is Phase 2 of that plan, not yet started.
