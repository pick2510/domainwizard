"""Groups several same-domain WRF/WPS NetCDF files (the common
frames_per_outfile=1 convention - one output file per timestep, e.g.
wrfout_d03_2025-03-14_00_00_00, ..._00_30_00, ...) into a single logical
"file" with one time axis spanning all of them.

WRFFileSeries deliberately exposes the exact same read-only surface as
wrfreader.WRFFile (.path/.name/.crs/.geotransform/.variables/.extra_dims/
.times/.read()) - every consumer (LayerRenderer, ViewForm) only ever touches
a WRFFile through that surface, so nothing downstream needs to know or care
whether it's backed by one file or many.

Lazy by default: opening a series only ever opens its *first* file up front
(needed immediately for its grid/variables/extra_dims), not all of them -
opening a real 164-variable/12-file run eagerly took ~17s by measurement,
almost entirely GDAL's per-variable metadata scan repeated per file, for
data that mostly never gets looked at (a user viewing one variable at a time
never needs 11 of those 12 files' metadata at all). Every other file is
opened - and, at that point, validated against the first file's grid/levels
- only the first time one of its timesteps is actually read(). This only
works because each file's own name already tells us its valid time (see
parse_wrf_filename) without opening it, so the whole time axis can be built
from filenames alone. The one case that still needs every file open
up front is a series whose files themselves contain more than one internal
timestep each (frames_per_outfile > 1) - the filename only gives that file's
*first* frame's time, so there's no way to know how many labels/mappings a
file contributes without reading it; this is the uncommon path, not the one
being optimized here.
"""
import re
from dataclasses import dataclass
from datetime import datetime
from typing import Dict, List, Optional, Tuple

from gis4wrf.core import UserError

from wrftools.wrfreader import WRFFile, WRFVariable

# Matches the WRF/WPS output naming conventions that carry both a domain
# number and a valid time: wrfout_d<NN>_<date>[_:.]<time>,
# wrfrst_d<NN>_<date>[_:.]<time>, met_em.d<NN>.<date>_<time>.nc. The
# date/time separator varies by installation - WRF's own default uses ':'
# for the time (HH:MM:SS), but some setups substitute '_' (filesystems/tools
# that dislike colons) - both are accepted, along with '.' after met_em's
# domain number. geo_em.* and wrfinput_d<NN> (no timestamp) never match, so
# they always open as single files, same as today.
_NAME_PATTERN = re.compile(
    r'^(?P<kind>wrfout|wrfrst|met_em)[._]d(?P<domain>\d{2})[._]'
    r'(?P<date>\d{4}-\d{2}-\d{2})[_.](?P<h>\d{2})[:_](?P<m>\d{2})[:_](?P<s>\d{2})'
    r'(?:\.nc)?$'
)

# Grid-equality tolerance for grid-mismatch checks (see _check_same_grid) -
# float geotransform components round-trip through the same corner-pixel
# lon/lat reads on every file of a real series, so any real mismatch is
# orders of magnitude larger than this.
_GEOTRANSFORM_TOLERANCE = 1e-6


@dataclass(frozen=True)
class ParsedWRFName:
    kind: str  # 'wrfout' | 'wrfrst' | 'met_em'
    domain: str  # e.g. '03'
    valid_time: datetime


def parse_wrf_filename(path: str) -> Optional[ParsedWRFName]:
    """Extracts (kind, domain, valid_time) from a WRF/WPS output filename,
    or None if it doesn't match a recognized series-forming pattern (a
    single static file like geo_em.d01.nc or wrfinput_d01 - no timestamp -
    always falls through to None, which callers treat as "open by itself")."""
    name = path.rsplit('/', 1)[-1]
    match = _NAME_PATTERN.match(name)
    if not match:
        return None
    try:
        valid_time = datetime(
            *(int(p) for p in match.group('date').split('-')),
            int(match.group('h')), int(match.group('m')), int(match.group('s')))
    except ValueError:
        return None
    return ParsedWRFName(kind=match.group('kind'), domain=match.group('domain'), valid_time=valid_time)


def group_paths(paths: List[str]) -> Tuple[List[List[str]], List[str]]:
    """Partitions `paths` into (groups, singles): `groups` holds every set
    of 2+ paths sharing a recognized (kind, domain) - each sorted by parsed
    valid_time (filename order as a tiebreak) - and `singles` holds every
    path that didn't match the naming pattern, or matched but was alone in
    its (kind, domain)."""
    by_key: Dict[Tuple[str, str], List[str]] = {}
    singles: List[str] = []
    for path in paths:
        parsed = parse_wrf_filename(path)
        if parsed is None:
            singles.append(path)
            continue
        by_key.setdefault((parsed.kind, parsed.domain), []).append(path)

    groups: List[List[str]] = []
    for group in by_key.values():
        if len(group) == 1:
            singles.append(group[0])
        else:
            groups.append(sorted(group, key=lambda p: (parse_wrf_filename(p).valid_time, p)))
    return groups, singles


class WRFFileSeries:
    """A time-ordered group of same-domain WRF/WPS files, exposing the same
    interface as wrfreader.WRFFile. `paths` must already be sorted (see
    group_paths) and contain 2 or more entries.

    Only `paths[0]` is opened here. .variables/.extra_dims/.crs/.geotransform
    are taken directly from it (skipping the cross-file intersection/
    consistency checks the previous eager version did up front) *when the
    fast lazy path applies* - see the module docstring. Every other file is
    opened, and checked against paths[0]'s grid, lazily in _file_at() the
    first time one of its timesteps is actually read - so a grid mismatch
    several files into a long series surfaces when the user steps to that
    time, not when the series is opened.
    """

    def __init__(self, paths: List[str]) -> None:
        self._paths = paths
        first = WRFFile(paths[0])
        self._files: Dict[int, WRFFile] = {0: first}

        self.path = first.path
        self.crs = first.crs
        self.geotransform = first.geotransform
        self.size = first.size

        parsed = [parse_wrf_filename(p) for p in paths]
        if len(first.times) == 1 and all(p is not None for p in parsed):
            # Fast path: every file's valid time (and, since the first file
            # has exactly one timestep, an assumed one-timestep-per-file
            # count) comes straight from its filename - no need to open
            # anything but paths[0]. .variables/.extra_dims come from that
            # one file only (not a true cross-file intersection - the rest
            # are never opened here at all).
            self.times = [p.valid_time.strftime('%Y-%m-%d %H:%M') for p in parsed]
            self._time_map = [(i, 0) for i in range(len(paths))]
            self.variables = first.variables
            self.extra_dims = first.extra_dims
        else:
            # Rare path: a file may contain more than one internal timestep,
            # which its filename alone can't tell us the count of - open
            # (and validate) everything up front, same as the non-lazy
            # version did unconditionally. Since every file is open anyway,
            # .variables is the real cross-file intersection here.
            for i in range(1, len(paths)):
                self._file_at(i)
            all_files = [self._files[i] for i in range(len(paths))]
            self.times, self._time_map = _build_stepped_times(all_files)
            self.variables = _intersect_variables(all_files)
            self.extra_dims = first.extra_dims

    def _file_at(self, file_index: int) -> WRFFile:
        if file_index not in self._files:
            wrf_file = WRFFile(self._paths[file_index])
            _check_same_grid(self._files[0], wrf_file)
            self._files[file_index] = wrf_file
        return self._files[file_index]

    @property
    def name(self) -> str:
        first_parsed = parse_wrf_filename(self._paths[0])
        prefix = f'{first_parsed.kind}_d{first_parsed.domain}' if first_parsed else self._files[0].name
        last_parsed = parse_wrf_filename(self._paths[-1])
        if first_parsed and last_parsed:
            span = f'{first_parsed.valid_time.strftime("%Y-%m-%d %H:%M")} - {last_parsed.valid_time.strftime("%Y-%m-%d %H:%M")}'
            return f'{prefix} ({len(self._paths)} files, {span})'
        return f'{prefix} ({len(self._paths)} files)'

    def read(self, var_name: str, time_index: int, level_index: int):
        file_index, local_time_index = self._time_map[time_index]
        return self._file_at(file_index).read(var_name, local_time_index, level_index)


def _check_same_grid(first: WRFFile, other: WRFFile) -> None:
    if other.crs.wkt != first.crs.wkt or other.size != first.size or not all(
            abs(a - b) <= _GEOTRANSFORM_TOLERANCE for a, b in zip(other.geotransform, first.geotransform)):
        raise UserError(
            f'{other.name} does not share the same grid as {first.name} - '
            'these files are not one WRF output series.')
    if other.extra_dims != first.extra_dims:
        raise UserError(
            f'{other.name} has different vertical/soil levels than {first.name} - '
            'these files are not one WRF output series.')


def _intersect_variables(files: List[WRFFile]) -> Dict[str, WRFVariable]:
    """The variables common to every file, in the first file's order - only
    used by the rare path above, where every file is already open."""
    common_names = set(files[0].variables)
    for f in files[1:]:
        common_names &= set(f.variables)
    return {name: files[0].variables[name] for name in files[0].variables if name in common_names}


def _build_stepped_times(files: List[WRFFile]) -> Tuple[List[str], List[Tuple[int, int]]]:
    """Step N of M labels concatenated across files - the fallback used only
    when some file has more than one internal timestep (see the class
    docstring for why real timestamps aren't available in that case)."""
    total = sum(len(f.times) for f in files)
    labels: List[str] = []
    time_map: List[Tuple[int, int]] = []
    step = 0
    for file_index, f in enumerate(files):
        for local_index in range(len(f.times)):
            step += 1
            labels.append(f'File {file_index + 1}, Step {local_index + 1} of {len(f.times)} ({step} of {total})')
            time_map.append((file_index, local_index))
    return labels, time_map
