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
# Adjust GDAL_DATA_DIR/PROJ_DB/FLEXIBLAS_DIR below if your system's paths
# differ (e.g. non-Fedora, or a different BLAS backend/no flexiblas at all,
# in which case drop that --add-binary line entirely).

set -euo pipefail
cd "$(dirname "$0")"

GDAL_DATA_DIR=$(gdal-config --datadir)
PROJ_DB=/usr/share/proj/proj.db
FLEXIBLAS_DIR=/usr/lib64/flexiblas

rm -rf build dist

uv run pyinstaller --onefile --name domainwizard --paths src \
  --add-binary "${FLEXIBLAS_DIR}:flexiblas" \
  --add-data "${GDAL_DATA_DIR}:share/gdal" \
  --add-data "${PROJ_DB}:share/proj" \
  src/domainwizard/app.py

echo
echo "Built: dist/domainwizard ($(du -h dist/domainwizard | cut -f1))"
