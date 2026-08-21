#!/bin/bash
# Builds a single-file executable with PyInstaller.
#
# Two things need bundling that PyInstaller's automatic dependency analysis
# doesn't catch, because both are loaded dynamically at runtime rather than
# via a normal Python import:
#
# - flexiblas backends (--add-binary): numpy's BLAS backend on this system
#   (Fedora) is flexiblas, which dlopen()s its actual backend implementation
#   (e.g. libflexiblas_openblas-openmp.so) based on a config file, invisible
#   to static analysis. Without this, the frozen binary aborts immediately
#   with "Failed to load the BLAS fallback library."
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
# flexiblas is Fedora-specific plumbing, not something inherent to numpy -
# it's absent entirely on other distros (confirmed: a plain manylinux_2_28
# container has no trace of it, and numpy works there without it), so this
# is only added if actually present on the build machine.
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
# Adjust GDAL_DATA_DIR/PROJ_DB below if your system's paths differ.

set -euo pipefail
cd "$(dirname "$0")"

GDAL_DATA_DIR=$(gdal-config --datadir)
PROJ_DB=/usr/share/proj/proj.db
FLEXIBLAS_DIR=/usr/lib64/flexiblas

rm -rf build dist

EXTRA_ARGS=()
if [ -d "$FLEXIBLAS_DIR" ]; then
  EXTRA_ARGS+=(--add-binary "${FLEXIBLAS_DIR}:flexiblas")
fi

uv run pyinstaller --onefile --name domainwizard --paths src \
  "${EXTRA_ARGS[@]}" \
  --add-data "${GDAL_DATA_DIR}:share/gdal" \
  --add-data "${PROJ_DB}:share/proj" \
  --hidden-import numpy.core._multiarray_umath \
  src/domainwizard/app.py

echo
echo "Built: dist/domainwizard ($(du -h dist/domainwizard | cut -f1))"
