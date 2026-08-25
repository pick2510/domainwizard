"""units.py: per-layer unit conversion for the View tab - pure numpy/stdlib,
no widgets involved."""
import numpy as np
import pytest

from wrftools import units


def test_native_unit_is_always_first_and_is_the_identity():
    options = units.conversions_for('K')
    assert options[0].key == 'native'
    assert options[0].scale == 1.0
    assert options[0].offset == 0.0


def test_unknown_native_unit_offers_only_the_identity():
    options = units.conversions_for('some-made-up-unit')
    assert len(options) == 1
    assert options[0].key == 'native'


def test_blank_and_category_units_offer_only_the_identity():
    assert len(units.conversions_for('')) == 1
    assert len(units.conversions_for('category')) == 1


def test_kelvin_to_celsius_freezing_point():
    degc = units.find('K', 'degC')
    assert units.convert(273.15, degc) == pytest.approx(0.0)


def test_kelvin_to_fahrenheit_boiling_point():
    degf = units.find('K', 'degF')
    assert units.convert(373.15, degf) == pytest.approx(212.0)


def test_m_s_to_knots():
    kn = units.find('m s-1', 'kn')
    assert units.convert(1.0, kn) == pytest.approx(1.9438444924406046)


def test_cf_style_unit_variants_normalize_the_same_as_m_s():
    for spelling in ('m s-1', 'm/s', 'ms-1', 'M S-1', '  m   s-1  '):
        options = units.conversions_for(spelling)
        assert {u.key for u in options} == {'native', 'kmh', 'kn', 'mph'}


def test_convert_works_on_arrays_too():
    degc = units.find('K', 'degC')
    result = units.convert(np.array([273.15, 373.15]), degc)
    np.testing.assert_allclose(result, [0.0, 100.0])


def test_round_trip_native_to_target_and_back_is_lossless():
    unit = units.find('Pa', 'hpa')
    native_value = 101325.0
    converted = units.convert(native_value, unit)
    back = (converted - unit.offset) / unit.scale
    assert back == pytest.approx(native_value)


def test_find_raises_for_a_unit_key_not_offered_by_this_native_unit():
    with pytest.raises(KeyError):
        units.find('K', 'knots')  # 'kn' is the real key, and K has no wind units anyway
