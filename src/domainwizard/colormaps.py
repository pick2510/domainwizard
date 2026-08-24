"""Named color LUTs for the View tab's per-layer colormap choice.

GIS4WRF exposes no colormap choice at all - continuous variables render as
plain QGIS greyscale (`plugin/geo.py:144 fix_style`), and its only color
handling is *categorical* GDAL color tables for LU_INDEX/WPS-categorical
datasets (`categories_to_gdal.py`, not vendored here). A per-layer colormap
is therefore new work, not a port - built with numpy only (no matplotlib
dependency) from a handful of anchor colors per map, linearly interpolated
into a 256-entry uint8 LUT. The anchor colors are hand-picked approximations
of the named perceptually-uniform maps (viridis/plasma/magma/cividis) and
common divergent/terrain/greyscale maps - not a pixel-exact reproduction of
matplotlib's versions, which isn't a goal here.

The vendored `gis4wrf.core.readers.categories` LANDUSE tables are reused for
a categorical LUT (LU_INDEX and friends), replacing GIS4WRF's GDAL
ColorTable + QGIS QgsPalettedRasterRenderer path with a plain numpy LUT
consistent with how continuous variables are colored here.
"""
from typing import Dict, List, Optional, Tuple

import numpy as np

from gis4wrf.core.readers.categories import LANDUSE

LUT_SIZE = 256

# Anchor colors (0-255 RGB) at evenly spaced positions from 0.0 to 1.0,
# interpolated per-channel into a 256-entry LUT by _build_continuous_lut.
_ANCHORS: Dict[str, List[Tuple[int, int, int]]] = {
    'viridis': [(68, 1, 84), (59, 82, 139), (33, 144, 140), (93, 201, 99), (253, 231, 37)],
    'plasma': [(13, 8, 135), (126, 3, 168), (204, 71, 120), (248, 149, 64), (240, 249, 33)],
    'magma': [(0, 0, 4), (81, 18, 124), (183, 55, 121), (252, 137, 97), (252, 253, 191)],
    'cividis': [(0, 32, 76), (58, 78, 99), (124, 123, 120), (192, 169, 102), (255, 234, 70)],
    'coolwarm': [(59, 76, 192), (146, 161, 214), (221, 221, 221), (212, 137, 116), (180, 4, 38)],
    'terrain': [(51, 102, 204), (51, 204, 102), (204, 204, 102), (153, 102, 51), (204, 204, 204), (255, 255, 255)],
    'greys': [(0, 0, 0), (255, 255, 255)],
}

# Fallback colors for a categorical value this app has no name/color for
# (e.g. an index outside the known scheme's range). Cycled deterministically
# by index rather than GIS4WRF's random-per-run choice, so re-rendering the
# same layer is reproducible.
_FALLBACK_CATEGORY_COLORS: List[Tuple[int, int, int]] = [
    (166, 206, 227), (31, 120, 180), (178, 223, 138), (51, 160, 44),
    (251, 154, 153), (227, 26, 28), (253, 191, 111), (255, 127, 0),
]


def names() -> List[str]:
    return list(_ANCHORS)


def _build_continuous_lut(anchors: List[Tuple[int, int, int]]) -> np.ndarray:
    anchor_positions = np.linspace(0.0, 1.0, num=len(anchors))
    sample_positions = np.linspace(0.0, 1.0, num=LUT_SIZE)
    anchors_arr = np.asarray(anchors, dtype=np.float64)
    lut = np.empty((LUT_SIZE, 3), dtype=np.uint8)
    for channel in range(3):
        lut[:, channel] = np.interp(sample_positions, anchor_positions, anchors_arr[:, channel]).round()
    return lut


_CONTINUOUS_LUT_CACHE: Dict[str, np.ndarray] = {}


def get(name: str) -> np.ndarray:
    """Returns the (256, 3) uint8 RGB LUT for a named continuous colormap."""
    if name not in _ANCHORS:
        raise KeyError(f'Unknown colormap {name!r}. Available: {names()}')
    if name not in _CONTINUOUS_LUT_CACHE:
        _CONTINUOUS_LUT_CACHE[name] = _build_continuous_lut(_ANCHORS[name])
    return _CONTINUOUS_LUT_CACHE[name]


def apply(values: np.ndarray, vmin: float, vmax: float, lut: np.ndarray) -> np.ndarray:
    """Maps a 2D float array through a (256, 3) RGB LUT into an (ny, nx, 4)
    RGBA uint8 array. NaN values (and a degenerate vmin==vmax range) become
    fully transparent."""
    ny, nx = values.shape
    rgba = np.zeros((ny, nx, 4), dtype=np.uint8)

    valid = np.isfinite(values)
    if not valid.any() or vmax <= vmin:
        return rgba

    normalized = np.clip((values - vmin) / (vmax - vmin), 0.0, 1.0)
    indices = np.zeros_like(normalized, dtype=np.intp)
    indices[valid] = (normalized[valid] * (LUT_SIZE - 1)).round().astype(np.intp)

    rgba[..., :3][valid] = lut[indices[valid]]
    rgba[..., 3][valid] = 255
    return rgba


def hex_to_rgb(hex_color: str) -> Tuple[int, int, int]:
    hex_color = hex_color.lstrip('#')
    return tuple(int(hex_color[i:i + 2], 16) for i in (0, 2, 4))


def categorical_lut(scheme: str, category_min: int, category_max: int) -> Tuple[np.ndarray, Dict[int, str]]:
    """Builds a (256, 3) uint8 LUT for a WRF landuse-style categorical
    variable, indexed directly by category value (values outside
    [category_min, category_max] map to black/unused). Returns the LUT and a
    {category_value: label} map for building a legend.

    Mirrors gis4wrf.core's (unvendored) categories_to_gdal.get_gdal_categories,
    but as a plain numpy LUT (this app colors continuous and categorical data
    through the same apply() path) rather than a GDAL ColorTable + QGIS
    paletted renderer, and with a deterministic fallback color instead of a
    random one.
    """
    known = LANDUSE.get(scheme, {})
    lut = np.zeros((LUT_SIZE, 3), dtype=np.uint8)
    labels: Dict[int, str] = {}

    for value in range(max(0, category_min), min(LUT_SIZE, category_max + 1)):
        if value in known:
            label, hex_color = known[value]
            lut[value] = hex_to_rgb(hex_color)
        else:
            label = f'Category {value}'
            lut[value] = _FALLBACK_CATEGORY_COLORS[value % len(_FALLBACK_CATEGORY_COLORS)]
        labels[value] = label

    return lut, labels


def apply_categorical(values: np.ndarray, lut: np.ndarray) -> np.ndarray:
    """Like apply(), but indexes the LUT directly by (rounded) category
    value instead of normalizing to [vmin, vmax]. Out-of-range or NaN values
    are transparent."""
    ny, nx = values.shape
    rgba = np.zeros((ny, nx, 4), dtype=np.uint8)

    valid = np.isfinite(values)
    indices = np.zeros_like(values, dtype=np.intp)
    indices[valid] = values[valid].round().astype(np.intp)
    in_range = valid & (indices >= 0) & (indices < LUT_SIZE)

    rgba[..., :3][in_range] = lut[indices[in_range]]
    rgba[..., 3][in_range] = 255
    return rgba
