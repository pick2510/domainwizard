# WRF Tools

A standalone, native C++20/Qt6/GDAL desktop app for working with WRF/WPS
files - no QGIS, no Chromium/QWebEngine, no third-party map library
required.

## Features

Five tabs; Domains, View, and Reproject share one map widget, LCZ and
Convert are plain file-in/file-out forms.

- **Domains** - define nested model domains by dragging/resizing directly
  on the map or editing numerically, with `namelist.wps` import/export.
- **View** - open WRF/WPS NetCDF files (`geo_em*`, `met_em*`, `wrfinput*`,
  `wrfout*`, including multi-file series auto-combined into one time axis)
  and raw WPS_GEOG binary datasets, and draw a configurable stack of
  colored raster layers over the basemap. Includes a movable colorbar,
  categorical legends (`LU_INDEX`, `IVGTYP`, soil type, ...), timestep
  playback, a north arrow, a pixel-value/lon-lat hover readout, and
  PNG/JPEG map export. Clicking a raster pixel opens a popup with either a
  timeseries of that point across every time step plus descriptive
  statistics (a series/multi-timestep file), or descriptive statistics and
  a distribution histogram of the whole raster (a single-timestep file).
- **Reproject** - convert wrfout files into plain CF-1.7 NetCDF on a
  regular grid in a chosen EPSG, for tools like QGIS, xarray/rioxarray, or
  CDO that don't understand WRF's native grid. Optionally crop to an AOI
  rectangle drawn on the map. Runs in a separate worker process so the GUI
  stays responsive.
- **LCZ** - applies Local Climate Zone urban-canopy parameters to a
  `geo_em` file from an LCZ classification GeoTIFF, for use with WRF's
  urban physics options.
- **Convert** - GeoTIFF <-> WPS geogrid binary conversion.

The app follows the OS's light/dark theme automatically (`Options > Theme`
for a manual override).

## Installation

Download the latest portable build for your platform from the
[Releases](../../releases) page - no installation needed, just extract and
run.

## Building from source

**Dependencies**

Debian/Ubuntu:

```sh
sudo apt install cmake ninja-build g++ qt6-base-dev libgdal-dev libproj-dev libnetcdf-dev pkg-config catch2
```

macOS (Homebrew):

```sh
brew install cmake ninja qt gdal proj netcdf pkg-config catch2
```

Windows (inside an MSYS2 MINGW64 shell):

```sh
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-qt6-base mingw-w64-x86_64-gdal mingw-w64-x86_64-proj \
  mingw-w64-x86_64-libtiff mingw-w64-x86_64-libgeotiff mingw-w64-x86_64-netcdf \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-catch
```

> GDAL >= 3.9 is required (for `exportToCF1`, used by the Reproject tab).
> On Ubuntu's stock apt (GDAL 3.8.4), add the `ubuntugis-unstable` PPA first.

**Build and run**

```sh
cmake -S . -B build-cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug
# On macOS, also pass -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build-cpp
ctest --test-dir build-cpp --output-on-failure
./build-cpp/wrftools
```

## Packaging

Every push/PR builds, tests, and packages the app for Linux, macOS, and
Windows via GitHub Actions, uploading portable bundles as workflow
artifacts. Tagged `v*` pushes publish a GitHub Release with all three
attached.

To build a portable bundle locally:

```sh
./build-portable-cpp.sh      # Linux -> dist/wrftools-cpp/
./build-portable-macos.sh    # macOS -> dist/wrftools.app, dist/wrftools-macos.zip
./build-portable-windows.sh  # Windows (MSYS2 MINGW64 shell) -> dist/wrftools-windows/, dist/wrftools-windows.zip
```

Each script bundles the app plus the `wrftools_reproject_worker` helper
and GDAL/PROJ data files into a self-contained, relocatable directory:

| Platform | Tool used | Notes |
|---|---|---|
| Linux | `cmake/bundle_linux.cmake` + `patchelf` | Full shared-library closure via `file(GET_RUNTIME_DEPENDENCIES)`, rewritten to `$ORIGIN`-relative RPATHs. Needs a comparably-recent-or-newer glibc than the build machine, and the target's own GL/EGL stack (not bundled - tied to the GPU driver). |
| macOS | `macdeployqt` + `dylibbundler` | Qt frameworks/plugins, then everything else (GDAL, PROJ, transitive deps). |
| Windows | `windeployqt` + hand-rolled `ldd` walk | Must run inside an MSYS2 MINGW64 shell. |

## Project layout

```
src/main.cpp, main_window.cpp   entry point + top-level window/tab set
src/tile_map_widget.cpp         shared map widget (tiles, overlays, drag/resize)
src/domain_form.cpp             Domains tab
src/domain_overlay.cpp          domain bboxes -> on-map outlines
src/domain.cpp                  domain bbox/padding/containment math
src/crs.cpp                     WRF/WPS CRS handling
src/wps_namelist.cpp            namelist.wps read/write
src/view_form.cpp               View tab
src/wrf_file.cpp, wrf_series.cpp, wrf_source.cpp, wps_binary_source.cpp
                                 WRF/WPS/WPS_GEOG file access
src/layer_renderer.cpp, raster_layer.cpp, warp.cpp
                                 View tab render/cache pipeline
src/point_inspector_dialog.cpp, chart_widget.cpp, stats.cpp
                                 click-a-pixel timeseries/distribution popups
src/lcz_form.cpp, lcz.cpp       LCZ tab + pipeline
src/reproject_form.cpp, reproject.cpp, reproject_worker.cpp
                                 Reproject tab + out-of-process worker
src/netcdf_file.cpp             netCDF-C RAII wrapper (used by LCZ + Reproject)
src/colormaps.cpp, units.cpp, colorbar.cpp
                                 color LUTs, unit conversion, legend rendering
src/theme.cpp                   light/dark theme detection
src/geotiff_convert_form.cpp    Convert tab (GUI over vendored convert_geotiff)
```

The LCZ pipeline runs synchronously on the GUI thread by design, not as an
oversight: `main.cpp`'s `GDALAllRegister()` thread-affines GDAL/HDF5 state
to the GUI thread, and running netCDF-C/GDAL from a second thread
afterward deadlocks on at least one real libhdf5 build (confirmed by
reproduction). The Reproject tab avoids the same hazard by running in a
separate process instead.

## Credits and license

WRF Tools itself is MIT licensed - see [LICENSE](LICENSE).

Parts of it are ported from, or vendor code from, other open-source
projects:

- **[gis4wrf](https://github.com/GIS4WRF/gis4wrf)** (MIT, (c) D. Meyer and
  M. Riechert, 2018) - this app's earlier Python/PyQt6 predecessor was
  built on gis4wrf's core; several pieces of geospatial logic (the WRF
  sphere CRS construction, WRF landuse/soil-type categorical color tables)
  are ported from `gis4wrf.core` into this app's C++ implementation.
- **[w2w](https://github.com/matthiasdemuzere/w2w)** (MIT, (c) Matthias
  Demuzere, 2021) - the LCZ tab's Stage 2-4 pipeline is a direct port of
  `w2w.py`/`add_wrf_version`'s `main()`.
- **[convert_geotiff](https://github.com/jbeezley/convert_geotiff)**
  (public domain, by jbeezley) - vendored unmodified in
  `src/convert_geotiff/`/`include/convert_geotiff/` except for its GUI, where
  the original FLTK interface was replaced with `GeotiffConvertForm`. The
  index/tile-data reader (no TIFF/GEOTIFF dependency) is built as a
  separate `convert_geotiff_reader` target so the View tab's
  `WpsBinarySource` can reuse it without pulling libtiff/libgeotiff into
  the core library.

## Known limitations

- No antimeridian-crossing or polar *display* domain support (a WPS_GEOG
  layer's own antimeridian wraparound is handled).
- No rotated lat/lon WRF/WPS projection support.
- No `albers_nad83` WPS_GEOG source projection support.
- Single-file internal times are index-labelled ("Step N of M") when GDAL
  can't expose a file's own `Times` variable; multi-file series use real
  timestamps unless a file in the series has more than one timestep itself.
- No checkerboard/grid-cell overlay - only domain outlines.
- A reprojected export drops WRF's own grid attributes, so it no longer
  opens in this app's own View tab (by design - other tools read the CF
  grid mapping correctly).
- Basemap uses OpenStreetMap's standard tile server.
