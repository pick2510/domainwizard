"""Per-layer unit conversion for the View tab: lets a layer be displayed in a
unit other than the one the WRF/WPS file itself stores (e.g. T2 in degC
instead of K, wind in knots instead of m/s).

Pure numpy/stdlib, no Qt and no GDAL - same shape as colormaps.py, so it's
directly unit-testable and rasterlayer.py's array-processing pipeline can
call it without pulling in any UI dependency.

Deliberately a small, explicit table rather than a general unit-parsing
library (pint, etc.): WRF/WPS output uses a small, fixed set of unit
strings (verified against tests/fixtures/*.nc and a real production wrfout -
see the View-tab plan), so a general parser would be a lot of unused
generality for a handful of known conversions.
"""
from dataclasses import dataclass
from typing import Dict, List, Union

import numpy as np

ArrayOrFloat = Union[np.ndarray, float]


@dataclass(frozen=True)
class Unit:
    key: str  # stable id stored on RasterLayer.units, e.g. 'degC'
    label: str  # shown in the dropdown and colorbar title, e.g. '°C'
    scale: float
    offset: float  # value_in_unit = value_in_native * scale + offset


def _native(label: str) -> Unit:
    """The identity conversion for a variable's own (native) unit - always
    first in conversions_for()'s result."""
    return Unit(key='native', label=label, scale=1.0, offset=0.0)


# Keyed by a normalized native unit string (see _normalize below). Each
# value is the list of *additional* (non-native) units offered for that
# native unit; conversions_for() prepends the native entry itself.
_CONVERSIONS: Dict[str, List[Unit]] = {
    'k': [
        Unit(key='degC', label='°C', scale=1.0, offset=-273.15),
        Unit(key='degF', label='°F', scale=9.0 / 5.0, offset=-459.67),
    ],
    'm s-1': [
        Unit(key='kmh', label='km/h', scale=3.6, offset=0.0),
        Unit(key='kn', label='knots', scale=1.9438444924406046, offset=0.0),
        Unit(key='mph', label='mph', scale=2.2369362920544025, offset=0.0),
    ],
    'pa': [
        Unit(key='hpa', label='hPa', scale=1.0 / 100.0, offset=0.0),
        Unit(key='inhg', label='inHg', scale=1.0 / 3386.389, offset=0.0),
    ],
    'm': [
        Unit(key='ft', label='ft', scale=3.280839895013123, offset=0.0),
        Unit(key='km', label='km', scale=1.0 / 1000.0, offset=0.0),
    ],
    'mm': [
        Unit(key='in', label='in', scale=1.0 / 25.4, offset=0.0),
    ],
    'kg m-2': [
        Unit(key='in', label='in', scale=1.0 / 25.4, offset=0.0),
    ],
}


def _normalize(native_units: str) -> str:
    """Folds the handful of equivalent spellings WRF/WPS/CF actually use
    (verified against real files - see the View-tab plan) onto one key:
    lowercase, single-spaced, with 'm/s'/'ms-1' treated the same as the
    CF-style 'm s-1' the fixtures use."""
    normalized = ' '.join(native_units.strip().lower().split())
    if normalized in ('m/s', 'ms-1', 'm.s-1'):
        return 'm s-1'
    return normalized


def conversions_for(native_units: str) -> List[Unit]:
    """Returns the available units for a variable whose file-native unit
    string is `native_units`: the native unit first (identity), then any
    known conversion targets. A single-element result (native only) is the
    signal callers use to hide the unit picker - nothing is convertible."""
    key = _normalize(native_units)
    return [_native(native_units)] + list(_CONVERSIONS.get(key, []))


def find(native_units: str, unit_key: str) -> Unit:
    """Looks up one Unit by its key among native_units's conversions_for()
    result. Raises KeyError if unit_key isn't one of them (a caller bug -
    e.g. a stale layer.units surviving a variable change)."""
    for unit in conversions_for(native_units):
        if unit.key == unit_key:
            return unit
    raise KeyError(f'Unknown unit {unit_key!r} for native unit {native_units!r}')


def convert(values: ArrayOrFloat, unit: Unit) -> ArrayOrFloat:
    """Converts `values` (an array or a single float, e.g. a range endpoint)
    from the variable's native unit into `unit`."""
    return values * unit.scale + unit.offset
