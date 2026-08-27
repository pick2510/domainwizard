#!/bin/bash
# Builds the native C++ app (Release) and bundles it into a self-contained,
# relocatable dist/wrftools-windows/ directory: bin/wrftools.exe, every DLL
# it (transitively) needs beyond the base Windows system set - Qt itself
# (via windeployqt) plus GDAL/PROJ/libtiff/libgeotiff/the MinGW runtime
# (walked by hand from `ldd`, since windeployqt only knows about Qt) - and
# GDAL/PROJ's data files under share/gdal, share/proj. main.cpp's
# configureGdalData() already looks for those two directories at
# ../share/gdal and ../share/proj relative to the executable, which is
# exactly this bundle's bin/ + share/ layout (mirroring
# build-portable-cpp.sh's Linux bundle), so no GDAL_DATA/PROJ_DATA
# environment setup is needed at run time. Also zips the bundle for easy
# upload/download, matching build-portable-macos.sh.
#
# Must run inside an MSYS2 MINGW64 shell (`C:\msys64\mingw64.exe`, or a CI
# step with `shell: msys2 {0}` after `msys2/setup-msys2` with
# `msystem: MINGW64`) - it builds against, and bundles DLLs from, that
# environment's own Qt6/GDAL/PROJ install
# (mingw-w64-x86_64-{qt6-base,gdal,proj,libtiff,libgeotiff}), not any
# MSVC/vcpkg toolchain.

set -euo pipefail
cd "$(dirname "$0")"

if [ "${MSYSTEM:-}" != "MINGW64" ]; then
  echo "This script must run inside an MSYS2 MINGW64 shell (MSYSTEM=MINGW64), not '${MSYSTEM:-<unset>}'." >&2
  exit 1
fi
MINGW_PREFIX="${MINGW_PREFIX:-/mingw64}"
WINDEPLOYQT="$MINGW_PREFIX/bin/windeployqt.exe"
if [ ! -x "$WINDEPLOYQT" ]; then
  echo "windeployqt not found at $WINDEPLOYQT - install mingw-w64-x86_64-qt6-base first." >&2
  exit 1
fi
if ! command -v zip >/dev/null; then
  echo "zip not found - install it first (\`pacman -S zip\`)." >&2
  exit 1
fi

BUILD_DIR=build-portable-windows
DIST_DIR=dist/wrftools-windows

rm -rf "$BUILD_DIR" "$DIST_DIR" "dist/wrftools-windows.zip"

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" -j"$(nproc)" --target wrftools_cpp

EXECUTABLE="$BUILD_DIR/wrftools.exe"
if [ ! -f "$EXECUTABLE" ]; then
  echo "Expected executable not found at $EXECUTABLE" >&2
  exit 1
fi

mkdir -p "$DIST_DIR/bin" "$DIST_DIR/share/gdal" "$DIST_DIR/share/proj"
cp "$EXECUTABLE" "$DIST_DIR/bin/"

# Deploys Qt's own DLLs (Core/Gui/Widgets/Network/...), the platform/image-
# format plugins, and the MinGW runtime DLLs (libgcc_s_seh, libstdc++,
# libwinpthread) it detects this is a MinGW build needs, all alongside the
# exe.
"$WINDEPLOYQT" --no-translations "$DIST_DIR/bin/wrftools.exe"

# windeployqt only resolves Qt's own dependency graph - GDAL/PROJ/libtiff/
# libgeotiff (and whatever they themselves need: libcurl, libsqlite3,
# libssl, zlib, ...) are still missing. Walked from `ldd` by hand: a
# breadth-first closure over every DLL under $MINGW_PREFIX (i.e. not a
# base-Windows system DLL, which `ldd` resolves to C:\Windows\...) that the
# exe or an already-collected DLL links against.
declare -A collected
queue=("$DIST_DIR/bin/wrftools.exe")
while IFS= read -r -d '' file; do queue+=("$file"); done < <(find "$DIST_DIR/bin" -maxdepth 1 -iname "*.dll" -print0)
while [ "${#queue[@]}" -gt 0 ]; do
  current="${queue[0]}"
  queue=("${queue[@]:1}")
  while IFS= read -r line; do
    dep_path=$(awk '{print $3}' <<<"$line")
    case "$dep_path" in
      "$MINGW_PREFIX"/*)
        dep_name=$(basename "$dep_path")
        if [ -z "${collected[$dep_name]:-}" ]; then
          collected[$dep_name]=1
          cp "$dep_path" "$DIST_DIR/bin/"
          queue+=("$DIST_DIR/bin/$dep_name")
        fi
        ;;
    esac
  done < <(ldd "$current" 2>/dev/null || true)
done

if [ -d "$MINGW_PREFIX/share/gdal" ]; then cp -R "$MINGW_PREFIX/share/gdal/." "$DIST_DIR/share/gdal/"; else
  echo "Warning: $MINGW_PREFIX/share/gdal not found - GDAL_DATA will not be bundled." >&2
fi
if [ -d "$MINGW_PREFIX/share/proj" ]; then cp -R "$MINGW_PREFIX/share/proj/." "$DIST_DIR/share/proj/"; else
  echo "Warning: $MINGW_PREFIX/share/proj not found - proj.db will not be bundled (CRS transforms will fail)." >&2
fi

(cd dist && zip -r -y -q wrftools-windows.zip wrftools-windows)

echo
echo "Built: $DIST_DIR and dist/wrftools-windows.zip ($(du -sh "$DIST_DIR" | cut -f1))"
echo "Run with: $DIST_DIR/bin/wrftools.exe"
