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
  CDO that don't understand WRF's native grid. A search box filters GDAL/
  PROJ's own EPSG catalog by name, area of use, or code so the target
  projection doesn't have to be looked up elsewhere and typed in by hand.
  Optionally crop to an AOI rectangle drawn on the map, and optionally
  compute extra output variables from a small arithmetic-processor script
  (e.g. `LVLHT = ((PH + PHB) / 9.81) - HGT;`, with arithmetic, comparisons,
  `?:`, and a small math function library). Runs in a separate worker
  process so the GUI stays responsive.
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
Windows via GitHub Actions, uploading portable bundles (plus a Linux
AppImage) as workflow artifacts. Tagged `v*` pushes publish a GitHub
Release with all of them attached.

To build a portable bundle locally:

```sh
./build-portable-cpp.sh      # Linux -> dist/wrftools-cpp/
./build-portable-appimage.sh # Linux AppImage (run after build-portable-cpp.sh) -> dist/wrftools-x86_64.AppImage
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
src/derived_variable.cpp        derived-variable script parser/evaluator
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

## Derived variables (Reproject tab)

The Reproject tab's "Derived Variables" box takes a small script (loosely
inspired by NCO's `ncap2`) that computes extra output variables from the
selected wrfout fields. Every name the script assigns is written to the
output automatically. Example (a real, commonly-used WRF post-processing
snippet):

```
LVLHT = (( PH + PHB ) / 9.81) - HGT;
LVLHT@units = "m";
LVLHT@long_name = "Height above ground [m]";
PTOT = (P + PB) * 0.01;
PTOT@long_name = "Total Level Pressure";
PTOT@units = "mbar";
TK = (PTOT/1000) ^ 0.2857 * (T + 300);
TK@units = "Kelvin";
TK@long_name = "Level Temperature [K]";
```

**Grammar** (standard operator precedence, lowest to highest):

```
script          := (statement ';')*
statement       := assignment | attrAssignment
assignment      := IDENT '=' expr
attrAssignment  := IDENT '@' IDENT '=' STRING

expr            := ternary
ternary         := logicalOr ('?' expr ':' expr)?      // right-associative; elementwise select
logicalOr       := logicalAnd ('||' logicalAnd)*
logicalAnd      := equality ('&&' equality)*
equality        := comparison (('=='|'!=') comparison)*
comparison      := additive (('<'|'<='|'>'|'>=') additive)*
additive        := term (('+'|'-') term)*
term            := power (('*'|'/'|'%') power)*
power           := unary ('^' power)?                  // right-associative
unary           := ('-'|'+'|'!')? primary
primary         := NUMBER | IDENT | IDENT '(' expr (',' expr)* ')' | '(' expr ')'
// '//' starts a line comment; NUMBER accepts an optional exponent (1e-3)
```

Every statement must end with `;`, including the last one. Comparison/
logical/`!` operators produce an elementwise 0.0/1.0 float array (so
`T2 * (T2 > 273.15)` works as a mask); `?:` elementwise-selects between its
two branches per pixel using the condition's 0.0/1.0 array. `@units`/
`@long_name` may only target a name already assigned earlier in the same
script.

**Builtin functions** (fixed arity, applied elementwise): `sqrt`, `exp`,
`log` (natural log), `log10`, `abs`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `floor`, `ceil`, `round`, `sign` (1 argument); `pow(x, y)`,
`atan2(y, x)`, `min(a, b)`, `max(a, b)` (2 arguments).

**A later statement may reference an earlier one's name** (chaining) - `TK`
above references `PTOT`, defined the line before it.

**Operands are read RAW, not destaggered** - unlike a directly-selected
pass-through variable (which this app auto-destaggers, e.g. a *_stag
variable averaged down to one fewer level), a derived expression's
operands come straight from the file exactly as stored. This is what makes
`LVLHT` above correct: `PH`/`PHB` keep their full staggered
`bottom_top_stag` level count, with the 2D `HGT` broadcasting across every
one of them - the standard geopotential-height-at-layer-interfaces formula
would be wrong on a destaggered `PH`/`PHB`. Combining two operands on
different named vertical dimensions (e.g. a `bottom_top` field directly
with a `bottom_top_stag` one) is rejected with a clear error; any number of
2D/scalar operands broadcast freely against at most one named dimension.

**A script can replace an original variable** - a source variable's own
name may be reassigned exactly once:

```
T2 = T2 - 273.15;
T2@units = "degC";
```

The RHS's own reference to `T2` still resolves to the ORIGINAL source
variable (not the new definition), so this is a replacement, not a
self-reference loop. The replacement supersedes any pass-through copy of
that name in the output - `T2` doesn't need to also be ticked in the
Variables list. If the script doesn't set `@units`/`@long_name` itself,
they fall back to the ORIGINAL variable's own - which is exactly why
`@units = "degC"` matters above: without it, the output would still carry
the stale `"K"` from the source even though the values are now Celsius.
Reassigning the same name a second time, or reassigning an already-derived
(non-source) name, is rejected as an error.

## Credits and license

WRF Tools itself is MIT licensed - see [LICENSE](LICENSE). This app was
built using two other open-source projects as a template - geospatial
logic and pipelines ported from their Python into this app's own C++:

- **[gis4wrf](https://github.com/GIS4WRF/gis4wrf)** (MIT, (c) D. Meyer and
  M. Riechert, 2018) - this app's earlier Python/PyQt6 predecessor was
  built on gis4wrf's core; several pieces of geospatial logic (the WRF
  sphere CRS construction, WRF landuse/soil-type categorical color tables)
  are ported from `gis4wrf.core` into this app's C++ implementation.
- **[w2w](https://github.com/matthiasdemuzere/w2w)** (MIT, (c) Matthias
  Demuzere, 2021) - the LCZ tab's Stage 2-4 pipeline is a direct port of
  `w2w.py`/`add_wrf_version`'s `main()`.

## Vendored code

`src/convert_geotiff/`/`include/convert_geotiff/` embeds
[convert_geotiff](https://github.com/jbeezley/convert_geotiff) (public
domain, by jbeezley) - specifically the author's own C++ port of the
original tool, which had already added an FLTK GUI. That GUI is what got
replaced with `GeotiffConvertForm` here; the conversion logic itself is
otherwise unmodified. The index/tile-data reader (no TIFF/GEOTIFF
dependency) is built as a separate `convert_geotiff_reader` target so the
View tab's `WpsBinarySource` can reuse it without pulling libtiff/libgeotiff
into the core library.

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
