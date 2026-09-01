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

# Recent macdeployqt (observed with Qt 6.11 on this CI's Homebrew) also
# tries to relink/re-sign every non-Qt dylib the executables pull in
# (GDAL's own now-large dependency tree - Arrow, the AWS SDK, OpenEXR,
# ...), and can hit a codesign verification error partway through that
# aborts its OWN later internal cleanup pass - observed leaving behind
# image-format/icon-engine/input-method PlugIns whose corresponding Qt
# framework (QtPdf/QtSvg/QtVirtualKeyboard - none of which this app
# actually uses) was never copied to Contents/Frameworks. A plugin like
# that is worse than simply absent: Qt will try to load it, get a
# resolution failure, and that's a second - separate from GDAL's own -
# reason a downloaded build could fail to start cleanly. Prune any
# PlugIns/**/*.dylib whose own @rpath Frameworks reference doesn't
# actually exist, rather than fighting macdeployqt's own incomplete
# pruning.
echo
echo "Pruning any plugin left referencing a framework macdeployqt never copied..."
find "$APP_PATH/Contents/PlugIns" -name "*.dylib" -type f -print0 2>/dev/null | while IFS= read -r -d '' plugin; do
  otool -L "$plugin" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '\.framework/' | while read -r dep; do
    framework_name="${dep#*/}"
    framework_name="${framework_name%%.framework/*}"
    if [ ! -d "$APP_PATH/Contents/Frameworks/${framework_name}.framework" ]; then
      echo "Removing $plugin - references ${framework_name}.framework, which was never bundled."
      rm -f "$plugin"
      break
    fi
  done
done

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

# dylibbundler (observed: version 1.0.5) adds "@executable_path/../libs"
# as an LC_RPATH entry once per dependency it relocates, not once per
# binary - so a binary with more than one relocated dependency ends up
# with the SAME rpath entry listed multiple times. Confirmed the hard
# way that this isn't cosmetic: dyld on this runner refuses to resolve
# ANY rpath-relative dependency on a binary with a duplicate LC_RPATH,
# reporting exactly the "Library not loaded ... (duplicate LC_RPATH ...)"
# failure a real user hit, even though the referenced file is genuinely
# present, correctly named, and correctly signed.
#
# Delete every existing copy of the entry and add it back exactly once.
# install_name_tool -delete_rpath needs the EXACT literal string dylibbundler
# wrote (a first attempt at this hardcoded "@executable_path/../libs" -
# without the trailing slash dylibbundler actually appends, confirmed by
# the error text itself: '@executable_path/../libs/' - so the delete call
# silently matched nothing and just added a THIRD, differently-spelled
# entry on top of the original duplicates). Read the real string back out
# of `otool -l` instead of assuming its exact spelling.
echo
echo "Deduplicating LC_RPATH entries dylibbundler may have added more than once..."
while IFS= read -r -d '' macho; do
  while true; do
    existing=$(otool -l "$macho" | awk '$1=="path" && $2 ~ /^@executable_path\/\.\.\/libs\/?$/ {print $2; exit}')
    [ -z "$existing" ] && break
    install_name_tool -delete_rpath "$existing" "$macho"
  done
  install_name_tool -add_rpath "@executable_path/../libs" "$macho"
done < <(find "$APP_PATH" -type f \( -name "*.dylib" -o -perm -u+x \) -print0)

mkdir -p "$APP_PATH/Contents/share"
GDAL_DATA_DIR="$(brew --prefix gdal)/share/gdal"
PROJ_DATA_DIR="$(brew --prefix proj)/share/proj"
if [ -d "$GDAL_DATA_DIR" ]; then cp -R "$GDAL_DATA_DIR" "$APP_PATH/Contents/share/gdal"; else
  echo "Warning: $GDAL_DATA_DIR not found - GDAL_DATA will not be bundled." >&2
fi
if [ -d "$PROJ_DATA_DIR" ]; then cp -R "$PROJ_DATA_DIR" "$APP_PATH/Contents/share/proj"; else
  echo "Warning: $PROJ_DATA_DIR not found - proj.db will not be bundled (CRS transforms will fail)." >&2
fi

# Both macdeployqt and dylibbundler rewrite install names/rpaths on
# dylibs Homebrew already ships ad-hoc-signed (install_name_tool prints
# "warning: changes being made to the file will invalidate the code
# signature" for every single one it touches), and a dylib with an
# invalid signature can fail to load - as plain "Library not loaded"
# with a "tried: ... (duplicate LC_RPATH ...)" reason, no distinct
# codesign-specific error text - even though the file is physically
# present and otherwise correct. This was confirmed the hard way: a
# single `codesign --force --deep --sign -` pass over the whole .app
# was NOT enough to fix it - `--deep` is documented by Apple as
# unreliable for anything beyond standard nested .framework/.app
# structure, which the loose dylibs dumped straight into Contents/libs
# by dylibbundler are not. Signing every Mach-O individually first
# (order doesn't matter for a plain ad-hoc signature - unlike a real
# Developer ID signature, it doesn't cryptographically validate nested
# code), then the outer .app bundle last, is what actually works.
echo
echo "Re-signing every Mach-O in the bundle individually (ad-hoc)..."
while IFS= read -r -d '' macho; do
  codesign --force --sign - "$macho"
done < <(find "$APP_PATH" -type f \( -name "*.dylib" -o -perm -u+x \) -print0)
codesign --force --sign - "$APP_PATH"

# ---- verify the bundle is actually loadable before shipping it ----
#
# Nothing above this point ever launches the built .app - a silent
# dylibbundler failure (a dependency it couldn't resolve/copy) previously
# shipped straight through to a tagged release, only surfacing for a real
# user as `dyld: Library not loaded: @executable_path/../libs/libgdal.*.dylib`
# at launch. Two checks below catch that class of bug here instead:
# statically, every @rpath/@executable_path/@loader_path reference in every
# bundled Mach-O must resolve to a file that's actually present in the
# bundle; then, as the most direct reproduction of the failure mode itself,
# both executables are actually launched and checked for a dyld error.
echo
echo "Verifying every bundled library reference resolves..."
missing=0
while IFS= read -r -d '' macho; do
  while IFS= read -r dep; do
    case "$dep" in
      @rpath/*|@executable_path/*|@loader_path/*)
        name="${dep##*/}"
        if ! find "$APP_PATH" -name "$name" -print -quit | grep -q .; then
          echo "MISSING: $macho references '$dep', but no file named '$name' exists anywhere in the bundle." >&2
          missing=1
        fi
        ;;
    esac
  done < <(otool -L "$macho" 2>/dev/null | tail -n +2 | awk '{print $1}')
done < <(find "$APP_PATH" \( -name "*.dylib" -o -perm -u+x \) -type f -print0)
if [ "$missing" -ne 0 ]; then
  echo "Portable bundle is missing one or more required libraries - see MISSING lines above." >&2
  exit 1
fi
echo "OK - every bundled library reference resolves to a file in the bundle."

echo
echo "Smoke-testing: launching the worker executable..."
WORKER_OUTPUT=$("$WORKER_EXECUTABLE" 2>&1 || true)
if echo "$WORKER_OUTPUT" | grep -qi "dyld\|Library not loaded"; then
  echo "Smoke test FAILED - the worker executable couldn't even start:" >&2
  echo "$WORKER_OUTPUT" >&2
  exit 1
fi
echo "OK - worker executable started (output: ${WORKER_OUTPUT:-<none>})."

echo
# Not QT_QPA_PLATFORM=offscreen: that plugin is a dev/test-only build of
# Qt this app deliberately doesn't bundle for real users, so forcing it
# here would fail on ANY correctly-packaged bundle, unrelated to whether
# dyld could load it - which is the only thing this check cares about.
# Cocoa may or may not fully initialize on a given CI runner regardless
# of packaging correctness, so - like the worker's own check above -
# look for dyld's own failure text rather than requiring the process to
# stay alive; a real dyld failure crashes before Qt gets anywhere near
# platform-plugin selection.
echo "Smoke-testing: launching the main executable..."
LAUNCH_LOG=$(mktemp)
"$EXECUTABLE" >"$LAUNCH_LOG" 2>&1 &
PID=$!
sleep 3
if kill -0 "$PID" 2>/dev/null; then
  kill "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  echo "OK - process launched and was still running after 3s."
else
  wait "$PID" 2>/dev/null || true
  if grep -qi "dyld\|Library not loaded" "$LAUNCH_LOG"; then
    echo "Smoke test FAILED - the main executable couldn't even start:" >&2
    cat "$LAUNCH_LOG" >&2
    rm -f "$LAUNCH_LOG"
    exit 1
  fi
  echo "OK - process started and exited without a dyld error (output below - a Qt platform-plugin/display issue here is a CI-environment limitation, not a packaging bug):"
  cat "$LAUNCH_LOG"
fi
rm -f "$LAUNCH_LOG"

(cd "$DIST_DIR" && zip -r -y -q wrftools-macos.zip wrftools.app)

echo
echo "Built: $DIST_DIR/wrftools.app and $DIST_DIR/wrftools-macos.zip"
echo "Run with: open \"$DIST_DIR/wrftools.app\""
