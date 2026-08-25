#!/bin/bash
# Builds a standalone executable with PyInstaller: --onefile (a single
# binary) on Linux, --onedir (a directory bundle, wrapped as a macOS .app
# by --windowed) on macOS - see the mode-selection comment below for why
# these need to differ per platform, unlike everything else in this script.
#
# Two things need bundling that PyInstaller's automatic dependency analysis
# doesn't catch, because both are loaded dynamically at runtime rather than
# via a normal Python import:
#
# - flexiblas backends (--add-binary, Linux only): numpy's BLAS backend on
#   Fedora-family distros is flexiblas, which dlopen()s its actual backend
#   implementation (e.g. libflexiblas_openblas-openmp.so) based on a config
#   file, invisible to static analysis. Without this, the frozen binary
#   aborts immediately with "Failed to load the BLAS fallback library."
#   flexiblas is that distro family's plumbing, not something inherent to
#   numpy - it's absent entirely on other distros (confirmed: a plain
#   manylinux_2_28 container has no trace of it, and numpy works there
#   without it) and on macOS (numpy uses Accelerate/OpenBLAS there instead,
#   linked normally, no dlopen indirection), so this is only added if
#   actually present on the build machine.
#
# - GDAL_DATA / PROJ_DATA (--add-data): GDAL/PROJ locate their data files
#   (projection definitions, proj.db) via these env vars, which app.py sets
#   at startup pointing into the bundle (see the comment there). We bundle
#   only proj.db (~10MB), not PROJ's full datum-grid directory (~775MB) -
#   this app only uses simple +datum=WGS84 proj4-string projections
#   (Lambert/Mercator/Polar/lat-lon), never grid-based datum shifts between
#   specific national reference frames, so proj.db alone is sufficient.
#   Without this, CRS transforms - which the whole domain-geometry pipeline
#   depends on - fail.
#
# - numpy.core._multiarray_umath (--hidden-import): osgeo's compiled
#   _gdal_array extension imports this directly from within its own C code,
#   not via any traceable Python source PyInstaller's static analysis can
#   see, so it's missed even though PyInstaller does otherwise understand
#   numpy (its own hook-numpy.py handles numpy's own imports fine). Without
#   this, anything touching GDAL's numpy array integration - which
#   gis4wrf.core.util imports unconditionally - fails with "ImportError:
#   numpy.core.multiarray failed to import".
#
# - nml_schemas/*.json (--add-data): read_namelist()/get_namelist_schema()
#   load these via a runtime-relative open(), not a Python import, so
#   they're invisible to PyInstaller's import-based bundling. Without this,
#   "Import from namelist" / "Export to namelist" fail with
#   "FileNotFoundError: .../nml_schemas/wps.json".
#
# proj.db's location isn't findable from gdal-config (it's a separate PROJ
# install, not a GDAL one) so it's located per-platform below: on Linux via
# the conventional system path, on macOS via `brew --prefix proj` since
# Homebrew's install prefix differs between Apple Silicon (/opt/homebrew)
# and Intel (/usr/local). Adjust GDAL_DATA_DIR/PROJ_DB if your system's
# layout differs from both (e.g. a non-Homebrew macOS GDAL install).

set -euo pipefail
cd "$(dirname "$0")"

GDAL_DATA_DIR=$(gdal-config --datadir)
FLEXIBLAS_DIR=/usr/lib64/flexiblas

# macOS needs both --onedir (NOT --onefile - see below) and --windowed:
#
# - --windowed makes PyInstaller wrap the build in a proper
#   dist/wrftools.app bundle (Info.plist, bundle identity, etc). Without
#   it, macOS never grants the process real foreground application/
#   window-owning status - the executable still runs (Qt's cocoa platform
#   plugin loads fine, the event loop spins), it just never gets an actual
#   window, silently. See the Packaging section of the README for how to
#   launch the result.
#
# - --onefile is actively wrong here, not just unnecessary: a onefile build
#   re-extracts its entire payload (every bundled GDAL/Qt/numpy .so/.dylib -
#   several hundred files) to a fresh temp directory on *every single
#   launch*, and macOS's ad-hoc-signature validation (dyld/AMFI) has no way
#   to cache "already validated" across launches for files at a new path
#   each time (unlike a real, once-installed .app, which it does cache).
#   Confirmed by hand: `sample` on the running (but windowless) process
#   showed ~99% of samples stuck in dyld's registerSignature()/fcntl() path
#   minutes after launch, and macOS eventually kills it outright (no crash
#   report - a managed termination, not a crash) for taking too long to
#   present a window. --onedir extracts once, at build time, into a stable
#   bundle layout instead, which is what avoids this entirely - PyInstaller
#   itself flags onefile+windowed on macOS as deprecated specifically
#   because of this clash with macOS's security model.
case "$(uname -s)" in
  Darwin)
    PROJ_DB="$(brew --prefix proj)/share/proj/proj.db"
    MODE_ARGS=(--onedir --windowed)
    ;;
  *)
    PROJ_DB=/usr/share/proj/proj.db
    MODE_ARGS=(--onefile)
    ;;
esac

rm -rf build dist

EXTRA_ARGS=()
if [ -d "$FLEXIBLAS_DIR" ]; then
  EXTRA_ARGS+=(--add-binary "${FLEXIBLAS_DIR}:flexiblas")
fi

# The `+` form below (rather than plain "${EXTRA_ARGS[@]}") is needed
# because macOS's default /bin/bash is stuck on 3.2 (last GPLv2 release) -
# under `set -u`, bash 3.2 treats expanding an *empty* array as an unbound
# variable and aborts, unlike bash 4+ (the Linux default). This form skips
# the expansion entirely when the array is empty, working on both. (Not
# needed for MODE_ARGS - it's never empty.)
uv run pyinstaller --name wrftools --paths src \
  "${MODE_ARGS[@]}" \
  "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" \
  --add-data "${GDAL_DATA_DIR}:share/gdal" \
  --add-data "${PROJ_DB}:share/proj" \
  --add-data "src/gis4wrf/core/readers/nml_schemas:gis4wrf/core/readers/nml_schemas" \
  --hidden-import numpy.core._multiarray_umath \
  src/wrftools/app.py

echo
if [ "$(uname -s)" = "Darwin" ]; then
  echo "Built: dist/wrftools.app ($(du -sh dist/wrftools.app | cut -f1))"
  echo "Run with: open dist/wrftools.app"
else
  echo "Built: dist/wrftools ($(du -h dist/wrftools | cut -f1))"
fi
