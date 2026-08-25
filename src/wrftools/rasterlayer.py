"""The View tab's layer model and rendering/caching pipeline: turns a
(file, variable, time, level, colormap, range) selection into a
wrftools.tilemap.RasterOverlay ready to hand to TileMapWidget.

Split into three cache tiers because the two expensive-vs-cheap operations
here differ by orders of magnitude in cost:

1. Open file handles (wrfreader.WRFFile) - avoids reopening/re-scanning a
   NetCDF file's variable list on every render.
2. The warped (EPSG:3857) float array for one (file, variable, time, level)
   slice - this is the expensive tier (a GDAL read + gdal.Warp reprojection).
   Bounded by total bytes, not entry count, since a slice can be anywhere
   from a few KB (a small geo_em crop) to tens of MB (a full wrfout field).
3. The colormapped RGBA QImage for one (slice, colormap, vmin, vmax)
   combination - cheap to rebuild from tier 2, but caching it means
   flipping colormaps or nudging the range redraws instantly with no
   re-warp.

Opacity, visibility, and the interpolate-on-display flag are deliberately
excluded from both cache keys (RasterLayer.slice_key/image_key) - they're
paint-time only (passed straight to RasterOverlay), so the opacity slider,
a layer's checkbox, and the interpolate toggle never invalidate anything.
"""
from dataclasses import dataclass
from typing import Dict, List, Optional, OrderedDict as OrderedDictType, Tuple, Union
from collections import OrderedDict

import numpy as np
from osgeo import gdal
from PyQt6.QtGui import QImage

from wrftools import colormaps, units as units_module
from wrftools.tilemap import RasterOverlay
from wrftools.wrfreader import WRFFile
from wrftools.wrfseries import WRFFileSeries

gdal.UseExceptions()

# Anything LayerRenderer can hold open and read from - a single WRFFile, or
# several combined into one WRFFileSeries (see wrfseries.py's module
# docstring for why the two are interchangeable everywhere below).
WRFFileLike = Union[WRFFile, WRFFileSeries]

SliceKey = Tuple[str, str, int, int]  # (file_path, variable, time_index, level_index)
# units is part of this key (unlike tick_count/tick_format/tick_decimals
# below) because it changes the rendered pixels - manual vmin/vmax are
# interpreted in the layer's displayed unit, and colormaps.apply() colors
# from the converted array.
ImageKey = Tuple[str, str, int, int, str, Optional[float], Optional[float], Optional[str]]

# Bytes budget for the warped-array cache (tier 2, the expensive one).
DEFAULT_SLICE_CACHE_BYTES = 256 * 1024 * 1024
# Entry-count budget for the colormapped-image cache (tier 3, cheap to rebuild).
DEFAULT_IMAGE_CACHE_SIZE = 48


@dataclass
class RasterLayer:
    layer_id: int
    file_path: str
    variable: str
    time_index: int = 0
    level_index: int = 0
    colormap: str = 'viridis'
    opacity: float = 0.8
    visible: bool = True
    interpolate: bool = True  # paint-time only, like opacity/visible - see below
    vmin: Optional[float] = None  # None => auto from the slice's own data; in *displayed* units
    vmax: Optional[float] = None
    units: Optional[str] = None  # target unit key (units.Unit.key); None => file-native unit
    # Colorbar legend appearance only - deliberately excluded from image_key()
    # (like opacity/visible/interpolate below): they never change the
    # rendered raster, so a tick tweak must not invalidate the image cache.
    tick_count: int = 3
    tick_format: str = 'auto'  # 'auto' (.3g) | 'fixed' | 'scientific'
    tick_decimals: int = 2

    def slice_key(self) -> SliceKey:
        return (self.file_path, self.variable, self.time_index, self.level_index)

    def image_key(self) -> ImageKey:
        return self.slice_key() + (self.colormap, self.vmin, self.vmax, self.units)

    def label(self) -> str:
        file_name = self.file_path.rsplit('/', 1)[-1]
        return f'{self.variable} — {file_name} (t={self.time_index + 1})'


@dataclass
class _SliceData:
    array: np.ndarray  # warped, EPSG:3857, float32, NaN for nodata
    bounds_3857: Tuple[float, float, float, float]
    auto_vmin: float
    auto_vmax: float

    def nbytes(self) -> int:
        return int(self.array.nbytes)


@dataclass
class _ImageData:
    image_rgba: np.ndarray  # kept alive: QImage(buffer) doesn't copy
    bounds_3857: Tuple[float, float, float, float]
    # Set only for a categorical layer (colormap == colormaps.CATEGORICAL) -
    # the swatch/label legend colorbar.build_categorical_legend_pixmap needs,
    # computed here so ViewForm's legend doesn't have to redo this work.
    categorical_lut: Optional[np.ndarray] = None
    categorical_labels: Optional[Dict[int, str]] = None
    present_categories: Optional[List[int]] = None


@dataclass
class RenderStats:
    slice_hits: int = 0
    slice_misses: int = 0
    image_hits: int = 0
    image_misses: int = 0


class LayerRenderer:
    """Renders RasterLayers into RasterOverlays, with the three-tier cache
    described in the module docstring."""

    def __init__(
        self,
        slice_cache_bytes: int = DEFAULT_SLICE_CACHE_BYTES,
        image_cache_size: int = DEFAULT_IMAGE_CACHE_SIZE,
    ) -> None:
        self._slice_cache_bytes = slice_cache_bytes
        self._image_cache_size = image_cache_size

        self._files: Dict[str, WRFFileLike] = {}
        self._slice_cache: 'OrderedDictType[SliceKey, _SliceData]' = OrderedDict()
        self._slice_cache_bytes_used = 0
        self._image_cache: 'OrderedDictType[ImageKey, _ImageData]' = OrderedDict()

        self.stats = RenderStats()

    # --- file handles ---------------------------------------------------

    def open_file(self, path: str) -> WRFFileLike:
        """Opens (or returns the already-open) file or file series. Raises
        UserError (via WRFFile/WRFFileSeries) if it isn't a recognized
        WRF/WPS NetCDF file, or (for a series key) was never registered by
        open_files()."""
        if path not in self._files:
            self._files[path] = WRFFile(path)
        return self._files[path]

    def open_files(self, paths: List[str]) -> WRFFileLike:
        """Opens one path as a plain WRFFile (identical to open_file), or
        several as one WRFFileSeries keyed by the group's first (earliest)
        path - see wrfseries.py. Raises UserError if 2+ paths don't form one
        coherent series (mismatched grid/levels) or aren't recognized
        WRF/WPS files at all."""
        if len(paths) == 1:
            return self.open_file(paths[0])
        series = WRFFileSeries(paths)
        if series.path not in self._files:
            self._files[series.path] = series
        return self._files[series.path]

    @property
    def open_paths(self) -> List[str]:
        return list(self._files)

    def invalidate_file(self, path: str) -> None:
        """Closes a file and drops every cached slice/image that came from
        it - for an explicit "close file" / "reload file" action, not
        automatic mtime polling."""
        self._files.pop(path, None)
        for key in [k for k in self._slice_cache if k[0] == path]:
            data = self._slice_cache.pop(key)
            self._slice_cache_bytes_used -= data.nbytes()
        for key in [k for k in self._image_cache if k[0] == path]:
            self._image_cache.pop(key)

    def clear(self) -> None:
        self._files.clear()
        self._slice_cache.clear()
        self._slice_cache_bytes_used = 0
        self._image_cache.clear()
        self.stats = RenderStats()

    # --- rendering --------------------------------------------------------

    def overlay_for(self, layer: RasterLayer) -> Optional[RasterOverlay]:
        """Returns a RasterOverlay for this layer's current selection, or
        None if the file isn't open. Never raises for a bad time/level index
        on an otherwise-valid variable - that's a caller bug, so it's
        allowed to propagate as WRFFile.read()'s UserError instead."""
        if layer.file_path not in self._files:
            return None

        image_data = self._get_image(layer)
        if image_data is None:
            return None

        # QImage(buffer, ...) wraps this exact array's memory without
        # copying - _buffer must be the SAME object passed into QImage, not
        # merely "an array with the same values", or the memory backing the
        # image could be freed out from under it once this function returns.
        rgba, image = _make_qimage(image_data.image_rgba)
        return RasterOverlay(
            image=image, bounds_3857=image_data.bounds_3857, opacity=layer.opacity,
            smooth=layer.interpolate, _buffer=rgba)

    def effective_range(self, layer: RasterLayer) -> Optional[Tuple[float, float]]:
        """Returns the (vmin, vmax) actually used to colormap this layer, in
        the layer's displayed unit - the manual override if set (already in
        displayed units - see RasterLayer.vmin/vmax), else the slice's own
        auto range converted from native units (the same computation
        _get_image does internally). Used by the View tab's on-map colorbar,
        which needs the range without needing a full colormapped image.
        Returns None if the file isn't open yet."""
        if layer.file_path not in self._files:
            return None
        slice_data = self._get_slice(layer)
        if slice_data is None:
            return None
        unit = self._unit_for(layer)
        auto_vmin, auto_vmax = self._auto_range_in_unit(slice_data, unit)
        vmin = layer.vmin if layer.vmin is not None else auto_vmin
        vmax = layer.vmax if layer.vmax is not None else auto_vmax
        return vmin, vmax

    def categorical_legend(self, layer: RasterLayer) -> Optional[Tuple[np.ndarray, Dict[int, str], List[int]]]:
        """Returns (lut, labels, present_categories) for a categorical
        layer's current selection - what colorbar.build_categorical_legend_pixmap
        needs - or None if the file isn't open, the slice can't be read, or
        the layer isn't using the categorical colormap."""
        if layer.file_path not in self._files or layer.colormap != colormaps.CATEGORICAL:
            return None
        image_data = self._get_image(layer)
        if image_data is None or image_data.categorical_lut is None:
            return None
        return image_data.categorical_lut, image_data.categorical_labels, image_data.present_categories

    def prefetch(self, layer: RasterLayer) -> None:
        """Populates the slice cache only (no colormapping/QImage) - used to
        warm neighbouring time steps on idle. A no-op on a cache hit."""
        if layer.file_path in self._files:
            self._get_slice(layer)

    def _get_slice(self, layer: RasterLayer) -> Optional[_SliceData]:
        key = layer.slice_key()
        cached = self._slice_cache.get(key)
        if cached is not None:
            self._slice_cache.move_to_end(key)
            self.stats.slice_hits += 1
            return cached

        self.stats.slice_misses += 1
        wrf_file = self._files[layer.file_path]
        array = wrf_file.read(layer.variable, layer.time_index, layer.level_index)
        warped_array, bounds_3857 = _warp_to_web_mercator(array, wrf_file.crs.wkt, wrf_file.geotransform)

        finite = warped_array[np.isfinite(warped_array)]
        if finite.size:
            auto_vmin, auto_vmax = float(finite.min()), float(finite.max())
        else:
            auto_vmin, auto_vmax = 0.0, 1.0

        data = _SliceData(array=warped_array, bounds_3857=bounds_3857, auto_vmin=auto_vmin, auto_vmax=auto_vmax)
        self._slice_cache[key] = data
        self._slice_cache_bytes_used += data.nbytes()
        self._evict_slices_if_needed()
        return data

    def _evict_slices_if_needed(self) -> None:
        while self._slice_cache_bytes_used > self._slice_cache_bytes and len(self._slice_cache) > 1:
            _, oldest = self._slice_cache.popitem(last=False)
            self._slice_cache_bytes_used -= oldest.nbytes()

    def _get_image(self, layer: RasterLayer) -> Optional[_ImageData]:
        key = layer.image_key()
        cached = self._image_cache.get(key)
        if cached is not None:
            self._image_cache.move_to_end(key)
            self.stats.image_hits += 1
            return cached

        self.stats.image_misses += 1
        slice_data = self._get_slice(layer)
        if slice_data is None:
            return None

        # Converted here, not in _get_slice(): the slice cache is keyed by
        # (file, variable, time, level) and shared across every layer built
        # from that same data, so converting units there would corrupt it
        # for a second layer viewing the same slice in a different unit (or
        # the native one).
        unit = self._unit_for(layer)
        array = units_module.convert(slice_data.array, unit) if unit.key != 'native' else slice_data.array

        if layer.colormap == colormaps.CATEGORICAL:
            wrf_file = self._files[layer.file_path]
            var = wrf_file.variables[layer.variable]
            finite = array[np.isfinite(array)]
            if finite.size:
                rounded = finite.round().astype(np.intp)
                cmin, cmax = int(rounded.min()), int(rounded.max())
                present = sorted(int(v) for v in np.unique(rounded))
            else:
                cmin, cmax, present = 0, 0, []
            lut, labels = colormaps.categorical_lut(var.category_scheme or '', cmin, cmax)
            rgba = colormaps.apply_categorical(array, lut)
            data = _ImageData(
                image_rgba=rgba, bounds_3857=slice_data.bounds_3857,
                categorical_lut=lut, categorical_labels=labels, present_categories=present)
        else:
            auto_vmin, auto_vmax = self._auto_range_in_unit(slice_data, unit)
            vmin = layer.vmin if layer.vmin is not None else auto_vmin
            vmax = layer.vmax if layer.vmax is not None else auto_vmax
            lut = colormaps.get(layer.colormap)
            rgba = colormaps.apply(array, vmin, vmax, lut)
            data = _ImageData(image_rgba=rgba, bounds_3857=slice_data.bounds_3857)

        self._image_cache[key] = data
        while len(self._image_cache) > self._image_cache_size:
            self._image_cache.popitem(last=False)
        return data

    def _unit_for(self, layer: RasterLayer) -> units_module.Unit:
        """The units.Unit this layer displays in - the file-native identity
        unit if layer.units is None or the variable is unknown/unopened."""
        wrf_file = self._files.get(layer.file_path)
        var = wrf_file.variables.get(layer.variable) if wrf_file else None
        native_units = var.units if var is not None else ''
        if layer.units is None:
            return units_module.Unit(key='native', label=native_units, scale=1.0, offset=0.0)
        return units_module.find(native_units, layer.units)

    @staticmethod
    def _auto_range_in_unit(slice_data: '_SliceData', unit: units_module.Unit) -> Tuple[float, float]:
        """Converts a slice's native-unit auto range into `unit`, sorting the
        result - a conversion with negative scale would otherwise swap min
        and max (none of units.py's shipped conversions do, but this removes
        the trap for a future one)."""
        lo = units_module.convert(slice_data.auto_vmin, unit)
        hi = units_module.convert(slice_data.auto_vmax, unit)
        return (lo, hi) if lo <= hi else (hi, lo)


def _warp_to_web_mercator(
    array: np.ndarray, src_wkt: str, src_geotransform: Tuple[float, ...],
) -> Tuple[np.ndarray, Tuple[float, float, float, float]]:
    """Builds a georeferenced in-memory GDAL dataset from a plain array and
    warps it to EPSG:3857, returning the warped array and its bounds. This
    is what makes the resulting overlay axis-aligned in tile space - the
    reason to reproject here rather than draw in the WRF file's native CRS
    (which would need a per-vertex/pixel reprojection in the paint path)."""
    ny, nx = array.shape
    src_ds = gdal.GetDriverByName('MEM').Create('', nx, ny, 1, gdal.GDT_Float32)
    src_ds.SetProjection(src_wkt)
    src_ds.SetGeoTransform(src_geotransform)
    band = src_ds.GetRasterBand(1)
    nan_mask = ~np.isfinite(array)
    filled = np.where(nan_mask, np.float32(-9999.0), array.astype(np.float32))
    band.WriteArray(filled)
    band.SetNoDataValue(-9999.0)

    warped_ds = gdal.Warp('', src_ds, format='MEM', dstSRS='EPSG:3857', resampleAlg='bilinear')
    warped = warped_ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    nodata = warped_ds.GetRasterBand(1).GetNoDataValue()
    if nodata is not None:
        # Note: bilinear resampling blends a true-nodata edge pixel with its
        # valid neighbor, so a thin border of values close to (but not
        # exactly) the sentinel can survive at the domain's true edge - this
        # only misses pixels there, not in the interior, and isn't visible
        # on any fixture used in testing (no interior nodata to blend with).
        # A tighter fix (nearest-neighbor for the alpha mask specifically)
        # is a possible refinement if a real domain edge shows a fringe.
        warped[np.isclose(warped, nodata)] = np.nan

    gt = warped_ds.GetGeoTransform()
    width, height = warped_ds.RasterXSize, warped_ds.RasterYSize
    minx = gt[0]
    maxy = gt[3]
    maxx = minx + gt[1] * width
    miny = maxy + gt[5] * height
    return warped, (minx, miny, maxx, maxy)


def _make_qimage(rgba: np.ndarray) -> Tuple[np.ndarray, QImage]:
    """Returns (array, image) where `image` wraps `array`'s memory directly -
    always use the returned array (not necessarily the input one) as the
    buffer to keep alive, since ascontiguousarray may return a copy."""
    rgba = np.ascontiguousarray(rgba)
    ny, nx, _ = rgba.shape
    image = QImage(rgba.data, nx, ny, nx * 4, QImage.Format.Format_RGBA8888)
    return rgba, image
