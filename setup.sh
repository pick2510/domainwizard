#!/bin/bash
# One-time (and re-run-after-pull) setup: pins the GDAL Python bindings to
# match this machine's installed libgdal exactly, then syncs the venv.
#
# GDAL's Python bindings enforce an exact-or-newer match against the system
# libgdal at build time (see their setup.py) - `pip`/`uv` picking whatever
# GDAL version resolves from a plain version constraint will fail on any
# machine whose libgdal doesn't happen to match. There's no version range
# that avoids this: it has to be pinned to what's actually installed, which
# differs per machine, so it can't just be a fixed value baked into
# pyproject.toml.
#
# Running this rewrites pyproject.toml/uv.lock's gdal pin for THIS machine.
# Don't commit that change on top of a pull meant for a different machine's
# environment - it's local, per-machine config, not a real dependency change.
#
# GDAL's setup.py also needs numpy importable *during its own build step* to
# compile the osgeo.gdal_array extension (silently skipped otherwise, no
# error - it just fails at import time later: "cannot import name
# '_gdal_array' from 'osgeo'"). uv builds each package in an isolated
# environment by default, which numpy - a separate, sibling dependency -
# isn't part of, so numpy has to be installed first and gdal's build told
# to use the main environment instead of an isolated one.

set -euo pipefail
cd "$(dirname "$0")"

if ! command -v gdal-config >/dev/null; then
    echo "gdal-config not found - install GDAL first:" >&2
    echo "  Fedora:        sudo dnf install gdal-devel" >&2
    echo "  Debian/Ubuntu: sudo apt install libgdal-dev" >&2
    echo "  macOS:         brew install gdal" >&2
    exit 1
fi

# On machines that also have Anaconda/Miniconda installed (common on
# meteorology workstations, this app's target audience), conda's own
# (often much older) gdal-config can shadow Homebrew's/the system's on
# PATH depending on shell init order, silently pinning to a stale GDAL.
case "$(command -v gdal-config)" in
    */conda*|*/anaconda*|*/miniconda*)
        echo "Note: using gdal-config from a conda environment ($(command -v gdal-config))." >&2
        echo "If that's not intended (e.g. you meant to use Homebrew's GDAL), adjust PATH" >&2
        echo "so the right gdal-config comes first, then re-run this script." >&2
        ;;
esac

uv sync --no-install-package gdal

GDAL_VERSION=$(gdal-config --version)
echo "Detected system libgdal $GDAL_VERSION, pinning Python bindings to match..."
uv add "gdal==${GDAL_VERSION}" --no-build-isolation-package gdal

echo "Done. Run with: uv run wrftools"
