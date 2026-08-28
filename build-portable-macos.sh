#!/bin/bash
# Builds the native C++ app (Release) and bundles it into a self-contained,
# relocatable dist/wrftools.app: Qt's own frameworks/plugins via Qt's own
# macdeployqt, plus every other non-system dylib (GDAL, PROJ, and their own
# transitive deps - libtiff, libcurl, libsqlite3, ...) via dylibbundler,
# plus GDAL/PROJ's data directories copied into Contents/share/{gdal,proj}.
# main.cpp's configureGdalData() already looks for those two directories at
# ../share/gdal and ../share/proj relative to the executable - which lives
# at Contents/MacOS/wrftools inside a .app bundle, so no GDAL_DATA/
# PROJ_DATA environment setup is needed at run time. Also zips the bundle
# for easy upload/download (a raw .app, being a directory, doesn't survive
# most file-transfer paths intact - notably GitHub Actions artifacts, which
# is what CI uses this script for).
#
# Unlike cmake/bundle_linux.cmake (a from-scratch dependency-closure walk,
# since Linux has no equivalent of macdeployqt), this leans on the two
# tools that are the de facto standard for this exact job on macOS rather
# than reimplementing what they already do well.
#
# Requires Homebrew Qt/GDAL/PROJ (`brew install qt gdal proj`) and
# dylibbundler (`brew install dylibbundler`). macdeployqt ships with the
# Homebrew qt formula itself.

set -euo pipefail
cd "$(dirname "$0")"

if ! command -v dylibbundler >/dev/null; then
  echo "dylibbundler not found - install it first (\`brew install dylibbundler\`)." >&2
  exit 1
fi
if ! command -v brew >/dev/null; then
  echo "Homebrew not found - this script only supports a Homebrew-based Qt/GDAL/PROJ install." >&2
  exit 1
fi

QT_PREFIX=$(brew --prefix qt)
MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
if [ ! -x "$MACDEPLOYQT" ]; then
  echo "macdeployqt not found at $MACDEPLOYQT - is the Homebrew qt formula installed?" >&2
  exit 1
fi

BUILD_DIR=build-portable-macos
DIST_DIR=dist

rm -rf "$BUILD_DIR" "$DIST_DIR"

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$QT_PREFIX"
# wrftools_reproject_worker: the Reproject tab launches this as a separate
# process at runtime (see reproject_form.hpp) - it has to ship inside the
# same .app bundle as the main executable, not just get built somewhere
# under $BUILD_DIR.
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)" --target wrftools_cpp wrftools_reproject_worker

# MACOSX_BUNDLE TRUE (CMakeLists.txt, APPLE-only) plus OUTPUT_NAME "wrftools"
# together always produce build-portable-macos/wrftools.app - not searched
# for, since a stray *.app from a leftover/unrelated CMake cache entry
# would otherwise silently get bundled instead.
APP_BUNDLE="$BUILD_DIR/wrftools.app"
if [ ! -d "$APP_BUNDLE" ]; then
  echo "Expected app bundle not found at $APP_BUNDLE - did the build actually produce a MACOSX_BUNDLE target?" >&2
  exit 1
fi

mkdir -p "$DIST_DIR"
cp -R "$APP_BUNDLE" "$DIST_DIR/"
APP_PATH="$DIST_DIR/wrftools.app"

EXECUTABLE="$APP_PATH/Contents/MacOS/wrftools"
if [ ! -x "$EXECUTABLE" ]; then
  echo "Expected executable not found at $EXECUTABLE" >&2
  exit 1
fi

# wrftools_reproject_worker (see reproject_form.hpp) has to live right
# beside the main executable in Contents/MacOS/ - that's where
# QCoreApplication::applicationDirPath() resolves to at runtime, and where
# workerExecutablePath() looks for it. Copied in BEFORE macdeployqt runs,
# below, so its own -executable= pass can fix up this binary's Qt
# references too, not just the main app's.
WORKER_EXECUTABLE="$APP_PATH/Contents/MacOS/wrftools_reproject_worker"
cp "$BUILD_DIR/wrftools_reproject_worker" "$WORKER_EXECUTABLE"

# -executable=: macdeployqt's own documented mechanism for an extra binary
# inside the bundle beyond the main one (e.g. a helper subprocess) that
# also links Qt and needs the same Frameworks/ relocation - without it, the
# worker's Qt6Core reference stays pointed at the build-time Homebrew path.
"$MACDEPLOYQT" "$APP_PATH" "-executable=$WORKER_EXECUTABLE"

# Bundles every remaining non-system dylib the executable (transitively)
# needs - GDAL/PROJ and everything under them - that macdeployqt doesn't
# touch (it only knows about Qt itself). dylibbundler recognizes libraries
# macdeployqt already relocated onto @rpath/@executable_path and leaves
# them alone, so running the two back to back is safe. Both binaries point
# at the same -d/-p destination, so a dependency needed by both (e.g.
# libproj) is only actually copied once - dylibbundler detects it's already
# present there on the worker's pass and just rewrites its reference.
dylibbundler -od -b \
  -x "$EXECUTABLE" \
  -d "$APP_PATH/Contents/libs" \
  -p "@executable_path/../libs"
dylibbundler -od -b \
  -x "$WORKER_EXECUTABLE" \
  -d "$APP_PATH/Contents/libs" \
  -p "@executable_path/../libs"

mkdir -p "$APP_PATH/Contents/share"
GDAL_DATA_DIR="$(brew --prefix gdal)/share/gdal"
PROJ_DATA_DIR="$(brew --prefix proj)/share/proj"
if [ -d "$GDAL_DATA_DIR" ]; then cp -R "$GDAL_DATA_DIR" "$APP_PATH/Contents/share/gdal"; else
  echo "Warning: $GDAL_DATA_DIR not found - GDAL_DATA will not be bundled." >&2
fi
if [ -d "$PROJ_DATA_DIR" ]; then cp -R "$PROJ_DATA_DIR" "$APP_PATH/Contents/share/proj"; else
  echo "Warning: $PROJ_DATA_DIR not found - proj.db will not be bundled (CRS transforms will fail)." >&2
fi

(cd "$DIST_DIR" && zip -r -y -q wrftools-macos.zip wrftools.app)

echo
echo "Built: $DIST_DIR/wrftools.app and $DIST_DIR/wrftools-macos.zip"
echo "Run with: open \"$DIST_DIR/wrftools.app\""
