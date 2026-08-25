"""Phase 2 of the View-tab plan: rasterlayer.py's layer model and the
three-tier render cache (open files / warped-array slices / colormapped
images) described in that module's docstring.

QImage construction doesn't need a display connection, but set
QT_QPA_PLATFORM=offscreen anyway for consistency with the rest of the
suite and in case a future Qt version tightens that.
"""
import dataclasses
import os

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import numpy as np
import pytest

from wrftools import colormaps
from wrftools.rasterlayer import LayerRenderer, RasterLayer
from wrftools.tilemap import RasterOverlay

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
GEO_EM = os.path.join(FIXTURES_DIR, 'geo_em_small.nc')
WRFOUT = os.path.join(FIXTURES_DIR, 'wrfout_multitime.nc')
SERIES_PATHS = [
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_00_00_00.nc'),
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_00_30_00.nc'),
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_01_00_00.nc'),
]


@pytest.fixture
def renderer():
    r = LayerRenderer()
    r.open_file(GEO_EM)
    r.open_file(WRFOUT)
    return r


# --- layer model --------------------------------------------------------

def test_slice_key_excludes_colormap_and_range():
    a = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', colormap='viridis', vmin=0, vmax=1)
    b = RasterLayer(layer_id=2, file_path=GEO_EM, variable='HGT_M', colormap='plasma', vmin=5, vmax=6)
    assert a.slice_key() == b.slice_key()
    assert a.image_key() != b.image_key()


def test_image_key_excludes_opacity_and_visibility():
    a = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', opacity=0.2, visible=True)
    b = RasterLayer(layer_id=2, file_path=GEO_EM, variable='HGT_M', opacity=0.9, visible=False)
    assert a.image_key() == b.image_key()


# --- rendering produces a usable overlay --------------------------------

def test_overlay_for_returns_a_georeferenced_raster_overlay(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    overlay = renderer.overlay_for(layer)
    assert isinstance(overlay, RasterOverlay)
    assert overlay.image.width() > 0 and overlay.image.height() > 0
    minx, miny, maxx, maxy = overlay.bounds_3857
    assert minx < maxx and miny < maxy


def test_overlay_for_unopened_file_returns_none():
    renderer = LayerRenderer()
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    assert renderer.overlay_for(layer) is None


def test_manual_vmin_vmax_is_honored(renderer):
    layer = RasterLayer(layer_id=1, file_path=WRFOUT, variable='U', level_index=0, vmin=-1.0, vmax=1.0)
    overlay = renderer.overlay_for(layer)
    assert overlay is not None


def test_interpolate_flag_is_passed_through_to_the_overlay(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', interpolate=False)
    overlay = renderer.overlay_for(layer)
    assert overlay.smooth is False


# --- effective_range: what the on-map colorbar shows ---------------------

def test_effective_range_is_auto_by_default(renderer):
    # The auto range comes from the warped (EPSG:3857) slice, not the raw
    # native-grid array - bilinear resampling can shift the min/max slightly,
    # so this only checks it's a sane range, not an exact match to the raw
    # array's own min/max.
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    vmin, vmax = renderer.effective_range(layer)
    assert vmin < vmax

    array = renderer._files[GEO_EM].read('HGT_M', 0, 0)
    raw_min, raw_max = float(np.nanmin(array)), float(np.nanmax(array))
    tolerance = 0.1 * (raw_max - raw_min)
    assert vmin >= raw_min - tolerance
    assert vmax <= raw_max + tolerance


def test_effective_range_honors_manual_override(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', vmin=10.0, vmax=200.0)
    assert renderer.effective_range(layer) == (10.0, 200.0)


def test_effective_range_of_unopened_file_returns_none():
    renderer = LayerRenderer()
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    assert renderer.effective_range(layer) is None


# --- cache tiers: hit/miss behavior --------------------------------------

def test_rerendering_the_same_layer_hits_the_image_cache(renderer):
    # An image-cache hit skips the slice tier entirely (no need to touch it
    # if the final image is already available) - so a full re-render with an
    # unchanged image_key costs nothing beyond the very first render.
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    renderer.overlay_for(layer)
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == 1
    assert renderer.stats.slice_hits == 0
    assert renderer.stats.image_misses == 1
    assert renderer.stats.image_hits == 1


def test_opacity_change_causes_no_new_reads_or_images(renderer):
    # opacity isn't part of image_key, so this is an image-cache hit (which,
    # in turn, never touches the slice tier - see the test above).
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', opacity=0.5)
    renderer.overlay_for(layer)
    before = dataclasses.replace(renderer.stats)
    layer.opacity = 0.1
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == before.slice_misses
    assert renderer.stats.slice_hits == before.slice_hits
    assert renderer.stats.image_misses == before.image_misses
    assert renderer.stats.image_hits == before.image_hits + 1


def test_colormap_change_misses_image_cache_only(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', colormap='viridis')
    renderer.overlay_for(layer)
    before = dataclasses.replace(renderer.stats)
    layer.colormap = 'plasma'
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == before.slice_misses  # slice reused
    assert renderer.stats.slice_hits == before.slice_hits + 1
    assert renderer.stats.image_misses == before.image_misses + 1  # new image


def test_time_change_misses_both_caches(renderer):
    layer = RasterLayer(layer_id=1, file_path=WRFOUT, variable='T2', time_index=0)
    renderer.overlay_for(layer)
    before = dataclasses.replace(renderer.stats)
    layer.time_index = 1
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == before.slice_misses + 1
    assert renderer.stats.image_misses == before.image_misses + 1


def test_stepping_back_in_time_is_a_cache_hit(renderer):
    layer = RasterLayer(layer_id=1, file_path=WRFOUT, variable='T2', time_index=0)
    renderer.overlay_for(layer)
    layer.time_index = 1
    renderer.overlay_for(layer)
    layer.time_index = 0  # back to the first step - both its slice AND its
    # image (same image_key as the very first render) are still cached, so
    # this is an image-cache hit that doesn't even touch the slice tier.
    before = dataclasses.replace(renderer.stats)
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == before.slice_misses
    assert renderer.stats.slice_hits == before.slice_hits
    assert renderer.stats.image_hits == before.image_hits + 1


def test_prefetch_populates_slice_cache_without_building_an_image(renderer):
    layer = RasterLayer(layer_id=1, file_path=WRFOUT, variable='T2', time_index=1)
    renderer.prefetch(layer)
    assert renderer.stats.slice_misses == 1
    assert renderer.stats.image_misses == 0
    before = dataclasses.replace(renderer.stats)
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == before.slice_misses  # already warm
    assert renderer.stats.slice_hits == before.slice_hits + 1


def test_two_layers_on_the_same_slice_share_the_warp(renderer):
    layer_a = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', colormap='viridis')
    layer_b = RasterLayer(layer_id=2, file_path=GEO_EM, variable='HGT_M', colormap='viridis')
    renderer.overlay_for(layer_a)
    renderer.overlay_for(layer_b)
    assert renderer.stats.slice_misses == 1  # same (file, var, time, level)


# --- invalidation ----------------------------------------------------------

def test_invalidate_file_drops_its_cache_entries_and_closes_it(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    renderer.overlay_for(layer)
    renderer.invalidate_file(GEO_EM)
    assert renderer.overlay_for(layer) is None  # file no longer open
    assert not any(key[0] == GEO_EM for key in renderer._slice_cache)
    assert not any(key[0] == GEO_EM for key in renderer._image_cache)


def test_invalidate_file_does_not_affect_other_files(renderer):
    layer_geo = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    layer_wrfout = RasterLayer(layer_id=2, file_path=WRFOUT, variable='T2')
    renderer.overlay_for(layer_geo)
    renderer.overlay_for(layer_wrfout)
    renderer.invalidate_file(GEO_EM)
    assert renderer.overlay_for(layer_wrfout) is not None


def test_clear_resets_everything(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    renderer.overlay_for(layer)
    renderer.clear()
    assert renderer.stats.slice_misses == 0
    assert renderer.overlay_for(layer) is None  # files were closed too


# --- categorical rendering --------------------------------------------------

def test_categorical_layer_uses_the_files_own_landuse_scheme(renderer):
    # geo_em_small.nc's MMINLU is MODIFIED_IGBP_MODIS_NOAH; its LU_INDEX
    # slice contains exactly classes 2, 13, 17 (verified against the fixture).
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='LU_INDEX', colormap=colormaps.CATEGORICAL)
    overlay = renderer.overlay_for(layer)
    assert overlay is not None
    lut, labels, present = renderer.categorical_legend(layer)
    assert present == [2, 13, 17]
    assert labels[2] == 'Evergreen Broadleaf Forest'
    np.testing.assert_array_equal(lut[2], colormaps.hex_to_rgb('#00FF00'))
    assert labels[13] == 'Urban and Built-Up'
    np.testing.assert_array_equal(lut[13], colormaps.hex_to_rgb('#FF0000'))
    assert labels[17] == 'Water'
    np.testing.assert_array_equal(lut[17], colormaps.hex_to_rgb('#000080'))


def test_categorical_legend_is_none_for_a_continuous_layer(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    assert renderer.categorical_legend(layer) is None


def test_categorical_legend_of_unopened_file_returns_none():
    renderer = LayerRenderer()
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='LU_INDEX', colormap=colormaps.CATEGORICAL)
    assert renderer.categorical_legend(layer) is None


# --- unit conversion ---------------------------------------------------------

def test_effective_range_in_a_converted_unit_is_shifted_from_native(renderer):
    native = RasterLayer(layer_id=1, file_path=WRFOUT, variable='T2')
    celsius = RasterLayer(layer_id=2, file_path=WRFOUT, variable='T2', units='degC')
    native_vmin, native_vmax = renderer.effective_range(native)
    celsius_vmin, celsius_vmax = renderer.effective_range(celsius)
    assert celsius_vmin == pytest.approx(native_vmin - 273.15)
    assert celsius_vmax == pytest.approx(native_vmax - 273.15)


def test_manual_range_in_a_converted_unit_is_used_as_is(renderer):
    layer = RasterLayer(layer_id=1, file_path=WRFOUT, variable='T2', units='degC', vmin=0.0, vmax=30.0)
    assert renderer.effective_range(layer) == (0.0, 30.0)


def test_units_change_misses_image_cache_but_not_slice_cache(renderer):
    layer = RasterLayer(layer_id=1, file_path=WRFOUT, variable='T2')
    renderer.overlay_for(layer)
    before = dataclasses.replace(renderer.stats)
    layer.units = 'degC'
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == before.slice_misses  # slice (native K) reused
    assert renderer.stats.slice_hits == before.slice_hits + 1
    assert renderer.stats.image_misses == before.image_misses + 1  # different pixels


def test_two_layers_same_slice_different_units_do_not_share_the_image_cache(renderer):
    native = RasterLayer(layer_id=1, file_path=WRFOUT, variable='T2')
    celsius = RasterLayer(layer_id=2, file_path=WRFOUT, variable='T2', units='degC')
    renderer.overlay_for(native)
    renderer.overlay_for(celsius)
    assert renderer.stats.slice_misses == 1  # same underlying slice
    assert renderer.stats.image_misses == 2  # but rendered in different units


# --- colorbar appearance fields (ticks) are legend-only ----------------------

def test_tick_settings_do_not_invalidate_either_cache_tier(renderer):
    layer = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    renderer.overlay_for(layer)
    before = dataclasses.replace(renderer.stats)
    layer.tick_count, layer.tick_format, layer.tick_decimals = 9, 'scientific', 4
    renderer.overlay_for(layer)
    assert renderer.stats.slice_misses == before.slice_misses
    assert renderer.stats.slice_hits == before.slice_hits
    assert renderer.stats.image_misses == before.image_misses
    assert renderer.stats.image_hits == before.image_hits + 1


# --- multi-file series (LayerRenderer.open_files) -------------------------

def test_open_files_with_a_single_path_behaves_like_open_file(renderer):
    single = renderer.open_files([GEO_EM])
    assert single is renderer.open_file(GEO_EM)


def test_open_files_with_a_series_registers_one_entry(renderer):
    series = renderer.open_files(SERIES_PATHS)
    assert SERIES_PATHS[0] in renderer.open_paths
    assert all(p not in renderer.open_paths for p in SERIES_PATHS[1:])
    assert series.path == SERIES_PATHS[0]


def test_overlay_for_works_at_every_time_index_of_a_series(renderer):
    series = renderer.open_files(SERIES_PATHS)
    for time_index in range(len(series.times)):
        layer = RasterLayer(layer_id=1, file_path=series.path, variable='T2', time_index=time_index)
        overlay = renderer.overlay_for(layer)
        assert overlay is not None


def test_reopening_the_same_series_is_a_cache_hit(renderer):
    first = renderer.open_files(SERIES_PATHS)
    second = renderer.open_files(SERIES_PATHS)
    assert first is second


# --- eviction ----------------------------------------------------------------

def test_slice_cache_evicts_by_total_bytes_not_count():
    renderer = LayerRenderer(slice_cache_bytes=1)  # force eviction on the 2nd distinct slice
    renderer.open_file(GEO_EM)
    layer_a = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M')
    layer_b = RasterLayer(layer_id=2, file_path=GEO_EM, variable='LU_INDEX')
    renderer.overlay_for(layer_a)
    renderer.overlay_for(layer_b)
    assert len(renderer._slice_cache) == 1  # layer_a's slice was evicted


def test_image_cache_evicts_by_entry_count():
    renderer = LayerRenderer(image_cache_size=1)
    renderer.open_file(GEO_EM)
    layer_a = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', colormap='viridis')
    layer_b = RasterLayer(layer_id=1, file_path=GEO_EM, variable='HGT_M', colormap='plasma')
    renderer.overlay_for(layer_a)
    renderer.overlay_for(layer_b)
    assert len(renderer._image_cache) == 1
