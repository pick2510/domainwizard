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
uv sync
```

That's it - no separate steps needed. `GDAL` is pinned to `3.12.4` to match
this machine's system `libgdal`; if `uv sync` fails to build it, check
`gdal-config --version` and adjust the pin in `pyproject.toml` to match.

## Run

```
uv run domainwizard
```

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

`src/gis4wrf/core/` is a copy of the `gis4wrf/core` subpackage from the
[GIS4WRF repo](https://github.com/GIS4WRF/gis4wrf) (domain/CRS math,
namelist read/write, GDAL outline generation - all QGIS-independent code).
It's vendored rather than depended on because the GIS4WRF repo isn't a
pip-installable package (it's zipped directly for QGIS's plugin loader, see
its `build.py`), so there's nothing for `uv add` to point at.

`src/gis4wrf/__init__.py` here is **not** a copy of the real
`gis4wrf/__init__.py` - that one wires up QGIS plugin bootstrapping
(`classFactory`, `bootstrap_with_ui`) which needs `qgis` and pulls in
`pkg_resources`/`setuptools` for its own dependency installer. None of that
applies here, so this package's `__init__.py` is just a docstring.

To pull in upstream changes, re-sync just the `core` subtree, e.g.:

```
rsync -a --exclude='__pycache__' /path/to/gis4wrf/gis4wrf/core/ src/gis4wrf/core/
```

## Known limitations

- Tile math and lon/lat projection is spherical-Mercator-for-display only;
  it hasn't been checked against antimeridian-crossing or polar domains.
- No checkerboard/grid-cell overlay (the QGIS plugin renders one via a GDAL
  VRT + pixel function built for QGIS's raster layer pipeline); domain
  *outlines* are rendered, which is what matters for checking nest
  placement.
- Basemap uses OpenStreetMap's standard tile server (the GIS4WRF plugin's
  Stamen-based basemap is dead - Stamen's tile servers were shut down).
