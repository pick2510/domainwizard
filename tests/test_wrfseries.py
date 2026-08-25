"""wrfseries.py: grouping same-domain WRF/WPS files by filename and
combining them into one WRFFileSeries with a unified time axis - no widgets
involved."""
import os

import pytest

from gis4wrf.core import UserError
from wrftools import wrfseries

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')
SERIES_PATHS = [
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_00_00_00.nc'),
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_00_30_00.nc'),
    os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_01_00_00.nc'),
]
GEO_EM = os.path.join(FIXTURES_DIR, 'geo_em_small.nc')
# Same size/CRS as the SERIES_PATHS fixtures but a genuinely different
# extent (different geotransform origin) - geo_em_small.nc happens to share
# the exact same grid as the series fixtures (both derived from the same
# real domain), so it can't be used to test grid-mismatch detection.
DIFFERENT_GRID = os.path.join(FIXTURES_DIR, 'wrfout_with_1d_var.nc')
# A copy of DIFFERENT_GRID's data under a name that *does* match the WRF
# naming pattern - unlike DIFFERENT_GRID itself, this makes a series built
# from it eligible for the fast lazy path, so it can be used to prove a
# mismatch several files into such a series is only caught once that file is
# actually read, not at construction time.
DIFFERENT_GRID_BUT_NAMED_LIKE_SERIES = os.path.join(FIXTURES_DIR, 'wrfout_d01_2020-01-01_02_00_00.nc')


# --- parse_wrf_filename ------------------------------------------------------

def test_parses_underscore_separated_time():
    parsed = wrfseries.parse_wrf_filename('wrfout_d03_2025-03-14_00_30_00')
    assert parsed.kind == 'wrfout'
    assert parsed.domain == '03'
    assert parsed.valid_time.isoformat() == '2025-03-14T00:30:00'


def test_parses_colon_separated_time():
    parsed = wrfseries.parse_wrf_filename('wrfout_d03_2025-03-14_00:30:00')
    assert parsed.valid_time.isoformat() == '2025-03-14T00:30:00'


def test_parses_met_em_naming():
    parsed = wrfseries.parse_wrf_filename('met_em.d01.2025-03-14_00:00:00.nc')
    assert parsed.kind == 'met_em'
    assert parsed.domain == '01'
    assert parsed.valid_time.isoformat() == '2025-03-14T00:00:00'


def test_files_without_a_timestamp_do_not_match():
    assert wrfseries.parse_wrf_filename('geo_em.d01.nc') is None
    assert wrfseries.parse_wrf_filename('wrfinput_d01') is None
    assert wrfseries.parse_wrf_filename('some_random_file.nc') is None


# --- group_paths --------------------------------------------------------

def test_groups_same_domain_files_together():
    groups, singles = wrfseries.group_paths(SERIES_PATHS)
    assert len(groups) == 1
    assert groups[0] == SERIES_PATHS  # already time-ordered
    assert singles == []


def test_a_lone_recognized_file_is_a_single_not_a_group_of_one():
    groups, singles = wrfseries.group_paths([SERIES_PATHS[0]])
    assert groups == []
    assert singles == [SERIES_PATHS[0]]


def test_unrecognized_files_are_singles():
    groups, singles = wrfseries.group_paths([GEO_EM])
    assert groups == []
    assert singles == [GEO_EM]


def test_different_domains_stay_in_separate_groups():
    d02_paths = [p.replace('_d01_', '_d02_') for p in SERIES_PATHS[:2]]
    groups, singles = wrfseries.group_paths(SERIES_PATHS + d02_paths)
    assert len(groups) == 2
    assert singles == []


def test_group_paths_sorts_by_valid_time_regardless_of_input_order():
    groups, _ = wrfseries.group_paths(list(reversed(SERIES_PATHS)))
    assert groups[0] == SERIES_PATHS


# --- WRFFileSeries -------------------------------------------------------

def test_series_times_are_real_formatted_timestamps_in_order():
    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    assert series.times == ['2020-01-01 00:00', '2020-01-01 00:30', '2020-01-01 01:00']


def test_series_read_matches_reading_the_underlying_file_directly():
    from wrftools.wrfreader import WRFFile
    import numpy as np

    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    direct = WRFFile(SERIES_PATHS[1]).read('T2', 0, 0)
    via_series = series.read('T2', 1, 0)
    np.testing.assert_array_equal(direct, via_series)


def test_series_name_describes_the_group():
    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    assert series.name == 'wrfout_d01 (3 files, 2020-01-01 00:00 - 2020-01-01 01:00)'


def test_series_path_is_the_earliest_files_path():
    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    assert series.path == SERIES_PATHS[0]


def test_series_variables_are_the_intersection_across_files():
    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    assert set(series.variables) == {'LU_INDEX', 'T2', 'U', 'V'}


def test_mismatched_grid_raises_usererror():
    with pytest.raises(UserError):
        wrfseries.WRFFileSeries([SERIES_PATHS[0], DIFFERENT_GRID])


# --- laziness ------------------------------------------------------------

def test_construction_only_opens_the_first_file():
    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    assert list(series._files) == [0]


def test_reading_a_new_file_opens_it_lazily():
    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    series.read('T2', 1, 0)
    assert set(series._files) == {0, 1}
    assert 2 not in series._files  # the third file is still untouched


def test_rereading_the_same_file_does_not_reopen_it():
    series = wrfseries.WRFFileSeries(SERIES_PATHS)
    first_open = series._files[0]
    series.read('T2', 0, 0)
    assert series._files[0] is first_open


def test_a_grid_mismatch_several_files_in_is_only_caught_when_read():
    paths = SERIES_PATHS + [DIFFERENT_GRID_BUT_NAMED_LIKE_SERIES]
    series = wrfseries.WRFFileSeries(paths)  # does not raise: only paths[0] is opened
    series.read('T2', 0, 0)  # the first 3 (matching) files read fine
    series.read('T2', 2, 0)
    with pytest.raises(UserError):
        series.read('T2', 3, 0)  # the 4th (mismatched) file is only checked now


def test_a_file_with_more_than_one_internal_timestep_falls_back_to_eager_open():
    # wrfout_multitime.nc's own filename doesn't match the series pattern at
    # all, so pairing it with a real series member exercises the "some file
    # can't tell us its timestep count from its name alone" fallback path,
    # not just "unparseable name" - both land in the same eager branch.
    multitime = os.path.join(FIXTURES_DIR, 'wrfout_multitime.nc')
    series = wrfseries.WRFFileSeries([multitime, SERIES_PATHS[0]])
    assert set(series._files) == {0, 1}  # both opened up front
