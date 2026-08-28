#!/bin/bash
# Builds the native C++ app (Release) and bundles it into a self-contained,
# relocatable directory at dist/wrftools-cpp/: the executable, every shared
# library it needs beyond the base-OS set (glibc/libstdc++/libgcc_s/...),
# the Qt platform plugin (dlopen'd at runtime, invisible to normal
# dependency-closure resolution), and GDAL/PROJ's data files. See
# cmake/bundle_linux.cmake for exactly what's bundled and why.
#
# Unlike build-portable.sh (the Python side), this does not use a Docker/
# old-glibc-baseline build: it promises portability across machines with a
# comparably-recent-or-newer glibc than the build machine, not "any glibc
# whatsoever". That covers "runs on other Debian/Ubuntu machines" without
# the extra Docker infrastructure - see cmake/bundle_linux.cmake's header
# comment for the reasoning.
#
# Requires patchelf (`apt install patchelf` on Debian/Ubuntu).

set -euo pipefail
cd "$(dirname "$0")"

if ! command -v patchelf >/dev/null; then
  echo "patchelf not found - install it first (e.g. \`apt install patchelf\` on Debian/Ubuntu)." >&2
  exit 1
fi

BUILD_DIR=build-portable-cpp
DIST_DIR=dist/wrftools-cpp

rm -rf "$BUILD_DIR" "$DIST_DIR"

# Force the system Qt6 package explicitly, restricted to /usr, rather than
# trusting CMake's default find_package search order: a machine with a
# second, non-system Qt6 install discoverable ahead of the system one
# (e.g. a self-built copy under $HOME) can otherwise get silently picked
# instead, producing a "portable" bundle that isn't actually built against
# the package this script's own dependency instructions assume.
# `find` exits non-zero if it hits so much as one permission-denied
# directory anywhere under /usr (very common on a real system), even though
# it still printed everything it could find to stdout. Piped into `head -1`
# under `set -o pipefail`, that non-zero status poisons the whole pipeline;
# assigned straight into a variable under `set -e`, that silently kills the
# entire script right here with no error message at all - `head` closing
# the pipe early makes this trivial to hit. `|| true` keeps the (correct)
# captured stdout while dropping that spurious pipeline failure; the
# empty-string check right below still catches a genuine "not found".
QT6_CMAKE_DIR=$(find /usr -maxdepth 6 -path "*/cmake/Qt6" -type d 2>/dev/null | head -1 || true)
if [ -z "$QT6_CMAKE_DIR" ]; then
  echo "Could not find a system Qt6 CMake package under /usr (looked for */cmake/Qt6)." >&2
  echo "Install it first (e.g. \`apt install qt6-base-dev\` on Debian/Ubuntu)." >&2
  exit 1
fi

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DQt6_DIR="$QT6_CMAKE_DIR"
# wrftools_reproject_worker: the Reproject tab launches this as a separate
# process at runtime (see reproject_form.hpp) - it has to ship in the same
# bundle as the main app, not just get built somewhere under $BUILD_DIR.
cmake --build "$BUILD_DIR" -j"$(nproc)" --target wrftools_cpp wrftools_reproject_worker

cmake -DEXECUTABLE="$BUILD_DIR/wrftools" -DWORKER_EXECUTABLE="$BUILD_DIR/wrftools_reproject_worker" -DOUTPUT_DIR="$DIST_DIR" -P cmake/bundle_linux.cmake

echo
echo "Built: $DIST_DIR ($(du -sh "$DIST_DIR" | cut -f1))"
echo "Run with: $DIST_DIR/bin/wrftools"
