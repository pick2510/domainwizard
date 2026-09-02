#!/bin/bash
# Wraps the relocatable bundle build-portable-cpp.sh already produces at
# dist/wrftools-cpp/ into a single-file AppImage: same bin/lib/share
# bundle, just laid out under AppDir's expected usr/{bin,lib,share} prefix
# (the bundle's own $ORIGIN-relative RPATHs and qt.conf's "../lib/qt6/
# plugins" are unaffected by that rename - they're relative to bin/, and
# bin/ stays exactly one level under usr/, same as it was under
# dist/wrftools-cpp/ directly), plus the .desktop/icon metadata AppImage
# itself requires, packaged with the upstream appimagetool.
#
# Requires build-portable-cpp.sh to have been run first (this script does
# NOT invoke it - keeps the two independently re-runnable, and avoids
# silently rebuilding a multi-minute native build every time you just want
# to repackage). Also requires curl (to fetch appimagetool on first run).

set -euo pipefail
cd "$(dirname "$0")"

DIST_DIR=dist/wrftools-cpp
WORK_DIR=build-portable-appimage
APPDIR="$WORK_DIR/wrftools.AppDir"
OUT_DIR=dist
APPIMAGETOOL="$WORK_DIR/appimagetool-x86_64.AppImage"

if [ ! -x "$DIST_DIR/bin/wrftools" ]; then
  echo "$DIST_DIR/bin/wrftools not found - run ./build-portable-cpp.sh first." >&2
  exit 1
fi

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr"
cp -r "$DIST_DIR/bin" "$DIST_DIR/lib" "$DIST_DIR/share" "$APPDIR/usr/"

mkdir -p "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cat > "$APPDIR/usr/share/applications/wrftools.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=wrftools
Comment=WRF/WPS domain, view, reprojection, and LCZ toolkit
Exec=wrftools
Icon=wrftools
Categories=Science;Geography;
Terminal=false
EOF
# AppImage also expects a copy of the .desktop file and icon at the AppDir
# root (not just the standard usr/share/ locations) - appimagetool refuses
# to build without them there.
cp "$APPDIR/usr/share/applications/wrftools.desktop" "$APPDIR/wrftools.desktop"
cp resources/wrftools.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/wrftools.png"
cp resources/wrftools.png "$APPDIR/wrftools.png"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/wrftools" "$@"
EOF
chmod +x "$APPDIR/AppRun"

if [ ! -x "$APPIMAGETOOL" ]; then
  echo "Downloading appimagetool..."
  curl -fsSL -o "$APPIMAGETOOL" "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
  chmod +x "$APPIMAGETOOL"
fi

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/wrftools-x86_64.AppImage"
# --appimage-extract-and-run: appimagetool is itself an AppImage, which
# normally mounts itself via FUSE - unavailable on most CI runners (no
# /dev/fuse in the container). This extracts it to a temp dir and runs
# the extracted binary directly instead, which needs no FUSE either way.
ARCH=x86_64 "$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$OUT_DIR/wrftools-x86_64.AppImage"

echo
echo "Built: $OUT_DIR/wrftools-x86_64.AppImage ($(du -sh "$OUT_DIR/wrftools-x86_64.AppImage" | cut -f1))"
