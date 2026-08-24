"""Phase 2 of the View-tab plan: colormaps.py - continuous and categorical
LUTs, no widgets involved."""
import numpy as np
import pytest

from domainwizard import colormaps


def test_all_named_colormaps_build_a_valid_lut():
    for name in colormaps.names():
        lut = colormaps.get(name)
        assert lut.shape == (256, 3)
        assert lut.dtype == np.uint8


def test_unknown_colormap_name_is_rejected():
    with pytest.raises(KeyError):
        colormaps.get('not-a-real-colormap')


def test_apply_maps_values_through_the_lut():
    lut = colormaps.get('viridis')
    values = np.array([[0.0, 10.0]])
    rgba = colormaps.apply(values, vmin=0.0, vmax=10.0, lut=lut)
    assert rgba.shape == (1, 2, 4)
    np.testing.assert_array_equal(rgba[0, 0, :3], lut[0])
    np.testing.assert_array_equal(rgba[0, 1, :3], lut[-1])
    assert (rgba[..., 3] == 255).all()


def test_apply_makes_nan_fully_transparent():
    lut = colormaps.get('viridis')
    values = np.array([[np.nan, 5.0]])
    rgba = colormaps.apply(values, vmin=0.0, vmax=10.0, lut=lut)
    assert rgba[0, 0, 3] == 0
    assert rgba[0, 1, 3] == 255


def test_apply_with_degenerate_range_is_fully_transparent():
    lut = colormaps.get('viridis')
    values = np.array([[1.0, 2.0]])
    rgba = colormaps.apply(values, vmin=5.0, vmax=5.0, lut=lut)
    assert (rgba[..., 3] == 0).all()


def test_apply_clips_out_of_range_values_to_the_ends():
    lut = colormaps.get('viridis')
    values = np.array([[-100.0, 100.0]])
    rgba = colormaps.apply(values, vmin=0.0, vmax=10.0, lut=lut)
    np.testing.assert_array_equal(rgba[0, 0, :3], lut[0])
    np.testing.assert_array_equal(rgba[0, 1, :3], lut[-1])


def test_categorical_lut_uses_known_landuse_colors():
    lut, labels = colormaps.categorical_lut('MODIFIED_IGBP_MODIS_NOAH', 1, 20)
    assert labels[1] == 'Evergreen Needleleaf Forest'  # a real label, not the unknown fallback
    np.testing.assert_array_equal(lut[1], colormaps.hex_to_rgb('#008000'))
    assert labels[2] == 'Evergreen Broadleaf Forest'
    np.testing.assert_array_equal(lut[2], colormaps.hex_to_rgb('#00FF00'))


def test_categorical_lut_falls_back_deterministically_for_unknown_scheme():
    lut, labels = colormaps.categorical_lut('SOME_UNKNOWN_SCHEME', 1, 5)
    assert all(labels[v] == f'Category {v}' for v in range(1, 6))
    lut2, _ = colormaps.categorical_lut('SOME_UNKNOWN_SCHEME', 1, 5)
    np.testing.assert_array_equal(lut, lut2)  # reproducible, not random per call


def test_apply_categorical_maps_by_direct_index():
    lut, _ = colormaps.categorical_lut('MODIFIED_IGBP_MODIS_NOAH', 1, 20)
    values = np.array([[1.0, 2.0], [np.nan, 999.0]])
    rgba = colormaps.apply_categorical(values, lut)
    np.testing.assert_array_equal(rgba[0, 0, :3], lut[1])
    np.testing.assert_array_equal(rgba[0, 1, :3], lut[2])
    assert rgba[1, 0, 3] == 0  # NaN
    assert rgba[1, 1, 3] == 0  # out of LUT range
