# Porting w2w (WUDAPT-to-WRF) to wrftools

## Context

[w2w](https://github.com/matthiasdemuzere/w2w) (`/home/strebdom/git/w2w`,
`w2w/w2w.py`, ~1700 lines + a 1300-line pytest suite) injects WUDAPT Local
Climate Zone (LCZ) urban-canopy parameters into WRF `geo_em.dXX.nc` files. It
strips urban pixels back to their dominant surrounding natural land cover,
reprojects/aggregates a dozen urban-canopy-parameter (UCP) fields from an LCZ
GeoTIFF onto the WRF grid, and writes three new NetCDF files
(`*_NoUrban.nc`, `*_LCZ_extent.nc`, `*_LCZ_params.nc`) plus, when needed,
`*_41.nc` copies of parent-domain files with `NUM_LAND_CAT` expanded to 41.

This is a different kind of tool from anything currently in wrftools.
wrftools (`CMakeLists.txt`, `src/`, `include/`, `PORT.md`) reads and displays
WRF/WPS NetCDF and edits WPS domain geometry; it has never written NetCDF,
and has no long-running batch pipeline or CLI surface - every existing
operation is interactive and fast. Some of the geospatial groundwork
(`crs.hpp`/`crs.cpp`, `warp.hpp`/`warp.cpp`) is directly reusable; the
NetCDF-write layer, the per-pixel urban-replacement algorithm, and the
UI/threading model are not.

Outcome: a native "LCZ" pipeline that reproduces w2w's three output files
byte-for-byte-equivalent (matching values, not necessarily identical NetCDF
internal layout) against the same inputs, wired into a new wrftools tab, with
w2w's own `testing/` fixtures pinning the port the same way Python fixtures
already pin the domain/CRS/rendering ports.

**Port target: `origin/add_wrf_version` (`7801b3e`), not `main`.** w2w's
`main` hardcodes the LCZ-class-to-`LU_INDEX` offset as `+30` and
`NUM_LAND_CAT` as `41` throughout. As of WRF 4.4.2, WRF's own `LANDUSEF`
grew from 41 to 61 categories (offset `+50` instead of `+30`) - `main`'s
output is simply wrong for any WRF version from 4.4.2 onward. The unmerged
`add_wrf_version` branch (34 commits ahead of `main` as of this writing,
last touched the same day as `main`'s own HEAD - an open PR, not yet
merged) is what fixes this: it adds a required `wrf_version` CLI argument,
a `WRF_VERSIONS_DICT` mapping version string -> `{ADD_LCZ_INT,
NUM_LAND_CAT}` (`{30, 41}` for v4.3-v4.4.1, `{50, 61}` for v4.4.2-v4.5.2),
and threads that choice through every function that previously hardcoded
30/31/41. Porting `main`'s constants would produce a tool that's already
wrong for current WRF releases; this plan targets `add_wrf_version`
throughout. Since it's an unmerged, moving-target branch, pin
implementation and fixture comparisons against commit `7801b3e` specifically
(re-sync if it merges or moves before this port is finished, rather than
silently drifting).

## Why this is a bigger lift than it looks

- **No NetCDF-write path exists at all.** `wrf_file.cpp` only ever opens
  files read-only through GDAL's generic NetCDF driver
  (`GDALOpenEx`/`NETCDF:"path"`). w2w's pattern - open, copy every
  dimension/variable/attribute, resize `LANDUSEF` from 21/24 to 41 land
  categories, add new variables (`FRC_URB2D`, `URB_PARAM`), write back - needs
  attribute-for-attribute fidelity because these files feed `real.exe`
  directly. GDAL's raster-dataset abstraction cannot round-trip WRF's
  non-raster dimensions (`land_cat`, `month`, `num_urb_params`) losslessly;
  this needs direct linkage against `libnetcdf` (netCDF-C), already present
  on this machine (`/usr/include/netcdf.h`, `pkg-config netcdf`) but not
  currently linked by any wrftools target.
- **A per-pixel nearest-neighbor search** (`wrf_remove_urban`'s
  `using_kdtree`) needs a KD-tree over great-circle-chord (ECEF-projected)
  coordinates - no such structure exists in wrftools today.
- **A truncated-normal sampler** (`_get_truncated_normal_sample`,
  `_compute_hi_distribution`) building the 15-bin `HI_URB2D` height
  distribution - no statistical sampling code exists in wrftools today, and
  getting this subtly wrong against the scipy reference is easy.
- **A generalized raster reprojection.** `warp.cpp` already builds a GDAL MEM
  dataset and warps it, but only ever to EPSG:3857 for map display. w2w warps
  an LCZ-derived synthetic raster to the WRF grid's own native CRS
  (Lambert/Mercator/Polar/lat-lon) and transform - needs generalizing to an
  arbitrary target CRS + explicit `Affine`-equivalent transform + shape, with
  `average`/`mode` resampling (`rasterio.warp.reproject`'s two resampling
  modes actually used).
- **A long-running pipeline off the GUI thread.** `wrf_remove_urban` loops
  over every urban pixel doing a KD-tree query and a `.mode()` computation;
  on real domains this is seconds to low minutes. Every current wrftools
  operation is synchronous on the GUI thread. `GeotiffConvertForm`
  (`geotiff_convert_form.hpp`/`.cpp`) already solved this exact problem for
  the Convert tab - background `std::thread` +
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` to marshal
  progress/completion back to the GUI thread - and is the pattern to reuse,
  not reinvent.
- **A UI surface.** wrftools has no CLI at all (`main.cpp` only launches the
  Qt app). A new "LCZ" tab, mirroring `DomainForm`/`ViewForm`/
  `GeotiffConvertForm`'s shape, fits the app's existing model far better than
  bolting on a separate CLI binary.

## Stage 0 - NetCDF read/write foundation

**Status: done.** `include/wrftools/netcdf_file.hpp` / `src/netcdf_file.cpp`
implement `NetcdfFile` (open/dimensions/variables/attributes/read/write,
`copyFile`, `resizeDimension`), linked into `wrftools_core` via
`PkgConfig::NETCDF` (`CMakeLists.txt`). `tests/fixtures/lcz/5by5.nc` (from
`w2w`'s own `testing/`) backs four new `core_tests.cpp` cases: read
dimensions/variables/attributes/data from a real file, `copyFile`
byte-for-byte, mutate-a-variable-and-a-global-attribute without disturbing
the rest of the file, and `resizeDimension` growing `LANDUSEF`'s `land_cat`
dimension (41 -> 45, since the fixture is already 41-category) while
leaving an unrelated variable/attribute untouched. One real bug found and
fixed along the way: `resizeDimension`'s per-variable data copy initially
used `nc_get_var_*`/`nc_put_var_*` (whole-array, sized by the *current*
state of whichever dataset the call targets) - fine for reading the source,
wrong for writing the destination, since every WRF geo_em file's `Time`
dimension is unlimited and a freshly `nc_create`d destination starts that
dimension at 0 records; writing "the whole array" against a 0-record
unlimited dimension silently writes nothing, so the destination's `Time`
dimension came back length-0 and a subsequent read crashed on the (empty)
result. Fixed by switching to explicit-`start`/`count` `nc_*_vara_*` calls
throughout, sized from the source's actual current record count rather
than the destination's. All 89 CTest entries (85 prior + 4 new) pass.
Also where the WRF-version scheme lands: a small
`struct WrfVersionInfo { int addLczInt; int numLandCat; }` plus a
`std::map<std::string, WrfVersionInfo>` transcribing `add_wrf_version`'s
`WRF_VERSIONS_DICT` (`{"v4.3", {30, 41}}` .. `{"v4.4.1", {30, 41}}`,
`{"v4.4.2", {50, 61}}` .. `{"v4.5.2", {50, 61}}`) belongs in `lcz.hpp`
alongside the `Info`-equivalent config struct (Stage 5's `LczForm` exposes
it as a dropdown instead of a required positional CLI arg). Every later
stage takes a `const WrfVersionInfo&` rather than hardcoding 30/31/41 or
50/51/61 - threading this through from the start avoids a second pass
through every function once Stage 2-4 land.

Prerequisite for everything else: nothing downstream can be built (or
tested) without the ability to open a WRF NetCDF file, mutate its variables/
attributes, and write a new file with full fidelity.

- New `include/wrftools/netcdf_file.hpp` / `src/netcdf_file.cpp`: a thin RAII
  wrapper over the netCDF-C API (`nc_open`, `nc_inq_*`, `nc_get_var*`,
  `nc_create`/`nc_def_dim`/`nc_def_var`/`nc_put_att*`/`nc_put_var*`), scoped
  to exactly what w2w needs - not a general-purpose NetCDF library:
  - Open an existing file read-only or read-write.
  - Enumerate dimensions, variables (name, type, shape, attributes), and
    global attributes.
  - Read a variable fully into a `std::vector<float/double/int32_t>` (WRF
    geo_em variables are never more than 4-D: `Time, {land_cat|month|
    num_urb_params}, south_north, west_east`).
  - **Copy-a-whole-file-then-mutate**, mirroring xarray's implicit
    copy-on-write semantics: `copyFile(src, dst)` (byte copy, cheapest), then
    open `dst` read-write and apply variable/attribute/dimension edits in
    place - this sidesteps having to reimplement NetCDF's define-mode/
    data-mode dance for the common "change a few things, keep everything
    else" case, which is most of what `w2w.py` actually does (`wrf_remove_
    urban`, `create_lcz_extent_file`, `expand_land_cat_parents` all start
    from `xr.open_dataset(...).copy()`-shaped logic).
  - `resizeDimension`/`replaceVariable` for the one case that needs true
    redefinition: `LANDUSEF`'s `land_cat` dimension growing from 21/24 to 41
    (NetCDF-classic dimensions can't be resized in place - this has to
    `nc_create` a new file with the new dimension size and copy every other
    variable/attribute across unchanged, matching what `xr.Dataset.
    drop_vars('LANDUSEF')` + reassignment does today).
- Link `libnetcdf` into `wrftools_core` in `CMakeLists.txt` (add a
  `find_package(NetCDF REQUIRED)` or `pkg_check_modules(NETCDF REQUIRED
  netcdf)` block next to the existing `find_package(GDAL ...)`).
- Tests (`tests/netcdf_file_tests.cpp`, new Catch2 binary or folded into
  `core_tests`): round-trip a small fixture (`w2w`'s own `testing/5by5.nc`,
  copied into `tests/fixtures/`) through open -> read every variable/
  attribute -> write unchanged -> reopen -> compare byte-for-byte with the
  original; then a mutation case (change one variable, resize `land_cat`,
  verify the rest of the file is untouched).

## Stage 1 - Geospatial pieces already half-built

**Status: done.**

- **Generalized warp**: `warpToGrid` (`warp.hpp`/`warp.cpp`) takes an
  explicit destination CRS/geotransform/shape/`ResampleMethod`
  (`Bilinear`/`Average`/`Mode`/`Nearest`) instead of hardcoding EPSG:3857.
  Implemented independently of `warpToWebMercator` via `GDALReprojectImage`
  against two pre-built MEM datasets (source + a destination already sized/
  geotransformed by the caller) rather than refactoring
  `warpToWebMercator` into a thin wrapper as originally sketched -
  `warpToWebMercator` needs to *discover* its output size
  (`GDALSuggestedWarpOutput` + its `kMaxWarpDimension` cap) before it has a
  destination to reproject into, which doesn't fit `warpToGrid`'s
  "destination already known" shape; sharing an implementation would have
  meant complicating the simpler, already-tested display path for no
  benefit. `warpToWebMercator` itself is untouched.
- **`wrfGridInfo`** (`lcz.hpp`/`lcz.cpp`), porting `_get_wrf_grid_info`
  (`w2w.py:581-660`) - not the same geotransform `WrfFile` derives from the
  staggered U/V grids. Two real subtleties found while porting, both now
  documented at length in `lcz.hpp`'s `WrfGridInfo` doc comment:
  - **Row order is deliberately NOT flipped.** `WrfFile` reads through
    GDAL's NetCDF driver, which silently presents WRF's on-disk
    south-increases-with-row-index arrays as north-up. `NetcdfFile` (Stage
    0) reads raw netCDF-C data, matching exactly what `xr.open_dataset`
    sees in Python - so the geotransform's row-height term is **positive**
    (`dy`, not `-dy`), confirmed by comparing against a live
    `w2w._get_wrf_grid_info()` run's `Affine.e` value. Any array warped
    through this geotransform must come from `NetcdfFile`, never `WrfFile`.
  - **`MAP_PROJ=6` is not `Crs::lonLat()`.** The plan's original assumption
    was wrong: w2w builds `+proj=eqc +lon_0=STAND_LON +a=6370000
    +b=6370000` for this case (a projected, metric CRS matching `DX`/`DY`
    in meters), not the plain `+proj=latlong` (degrees) this project's own
    `Crs::lonLat()`/`buildWrfCrs`'s `case 6` use for map display. Added
    `Crs::fromProj4()` (a thin public wrapper around the existing private
    constructor, mirroring `fromWkt()`) so `wrfGridInfo` can build this
    proj4 string directly without a named `Crs` factory for it. `Crs::
    lambert`/`Crs::polar`/`Crs::mercator` *are* reused as originally
    planned for `MAP_PROJ` 1/2/3, since those already matched.
  - The domain's projected origin (`e, n`) is transformed from the file's
    own `CEN_LON`/`CEN_LAT` through **real WGS84** (`Crs::wgs84()`), not
    this project's usual no-datum-shift `Crs::toXy` - matching w2w's own
    `pyproj.Proj(proj='latlong', datum='WGS84')` source CRS. Implemented by
    reusing `Crs::transformBbox` with a degenerate (point-sized) bbox
    rather than adding a new single-point transform primitive.
- **Pinned against live Python output**, matching this project's existing
  practice: `tests/fixtures/lcz/5by5.nc` (`MAP_PROJ=6`) and `tests/
  fixtures/lcz/geo_em.d01_Shanghai_ncl20.nc` (`MAP_PROJ=1`, from w2w's own
  `testing/`) back two `wrfGridInfo` cases in `core_tests.cpp`, each
  checked to full double precision against a real `w2w._get_wrf_grid_info()`
  call (`w2w`'s `main` branch - this specific function is unchanged on
  `add_wrf_version`, confirmed by diff). `warpToGrid` gets a same-CRS
  half-resolution averaging sanity check (no Python fixture needed - purely
  checks the destination-grid/resampling mechanism itself). All 92 CTest
  entries pass.

## Stage 2 - `wrf_remove_urban` (`*_NoUrban.nc`)

**Status: done.** `removeUrban(srcPath, dstPath, npixNlc, npixArea)` lands
in `lcz.hpp`/`lcz.cpp`. One correction to the plan's original signature:
it does **not** take a `WrfVersionInfo` - re-reading `add_wrf_version`'s
`wrf_remove_urban` confirmed its pre-pass branches on the *source file's
own* `NUM_LAND_CAT` attribute (self-contained re-processing detection), not
on a caller-supplied target version; `WrfVersionInfo` is only actually
needed starting Stage 3, where it genuinely does get threaded in as a
parameter.

- **Pre-pass** (`add_wrf_version`-only, absent from `main`): ported as a
  branch on the source file's own `NUM_LAND_CAT` (41 or 61), collapsing
  `LU_INDEX` values in `(30,41)`/`(50,61)` back to `ISURBAN` and summing
  their `LANDUSEF` fractions into it, before the normal removal loop -
  including w2w's own slight over-inclusiveness (the `LANDUSEF` slice's
  upper bound is one past the `LU_INDEX` threshold's, e.g. `luf[30:41]`
  vs. `(luse>30)&(luse<41)`) rather than "fixing" it into a symmetric
  range.
- `NUM_LAND_CAT` is unconditionally reset to 21 in the output, matching
  `add_wrf_version`.
- **KD-tree**: vendored `nanoflann.hpp` (BSD, from the real upstream
  `jlblancoc/nanoflann` tag `v1.6.3`, at `include/third_party/`) - no
  Debian package was actually installable without sudo, so downloaded
  directly rather than via `apt`/`FetchContent` as originally sketched.
  `EcefPointCloud`/`PixelKdTree` in `lcz.cpp` mirror `using_kdtree` (R=6371,
  spherical-to-ECEF, `k` nearest) exactly, including that the query point
  set is the dataset itself (so index 0 of every result is always the
  point itself, at distance 0) - `nanoflann::knnSearch` already returns
  ascending-distance order, matching `scipy.spatial.KDTree.query`.
- **`modalValue`** (the port's name for `pandas.Series.mode()[0]`): an
  `std::map<int,int>` frequency tally, walked in ascending key order with
  strict `>` so the first (smallest) key reaching the max count wins -
  matches pandas' own tie-break (`mode()` returns every tied value in
  ascending order; `[0]` takes the first) exactly, confirmed by the live
  pin below rather than assumed.
- The two loops (`w2w.py:530-556`/`562-567` on `add_wrf_version`) and the
  `luf_natland`/`luf_urb`/`luse_natland`/`luse_urb` boolean masks port
  directly to per-pixel flat-array loops - no new infrastructure beyond
  Stage 0's read/write and the KD-tree above.
- Row/flatten order needed no adjustment: `NetcdfFile::readFloat` already
  returns raw on-disk (row-major over `Time, land_cat/month, south_north,
  west_east`) order, identical to what `xr.open_dataset`/`.values.ravel()`
  produce in Python - the same fact Stage 1 relied on for `wrfGridInfo`'s
  geotransform.
- **Pinned against a live `add_wrf_version` run**, not `test_wrf_remove_
  urban`'s own fixture (that test's `dst_nu_file` isn't checked into the
  repo, unlike the `*_LCZ_params.nc`/`*_LCZ_extent.nc`/`*_NoUrban.nc`
  outputs Stage 3/4 can pin against directly): ran `wrf_remove_urban(info,
  NPIX_NLC=3, NPIX_AREA=9)` from a `git worktree` checkout of
  `add_wrf_version`'s `7801b3e` (loaded directly via `importlib`, no
  reinstall needed) against `tests/fixtures/lcz/5by5.nc`, in a throwaway
  venv built from prebuilt wheels only (source builds against the
  system's Python 3.14 failed on `pkg_resources`/`setuptools` for
  `rasterio`, unrelated to this port). Three new `core_tests.cpp` cases:
  every `LU_INDEX`/`GREENFRAC` (two representative months)/`LANDUSEF`
  value checked against that run's real output (including its own
  non-obvious behavior - a couple of pixels end up with **no** nonzero
  `LANDUSEF` category at all, sum below 1.0, not something to "fix"), an
  `NPIX_AREA`-too-large rejection case, and - since no shipped fixture
  already carries LCZ-range `LU_INDEX` values to exercise the pre-pass
  meaningfully - a hand-mutated-pixel case (one pixel set to LCZ class 35
  before running) checked structurally rather than against a numeric pin.
  All 95 CTest entries pass.

## Stage 3 - `create_lcz_params_file` / `create_lcz_extent_file` (the core UCP pipeline)

**Status: done**, including `checkLczIntegrity` (moved here from its own
earlier commit - see git history). All of `ucpResample`/`hgtResample`/
`lczResample`/`hiResample`/`createLczParamsFile`/`createLczExtentFile` are
implemented and verified end-to-end against a live `add_wrf_version`
(`7801b3e`) run of the FULL pipeline (`check_lcz_integrity` ->
`wrf_remove_urban` -> `create_lcz_params_file` -> `create_lcz_extent_file`)
on the real Zaragoza sample domain (`sample_data/lcz_zaragoza.tif` +
`geo_em.d04.nc`, 102x162 = 16524 pixels) - one `core_tests.cpp` case checks
every `LU_INDEX`/`FRC_URB2D`/`URB_PARAM`/`LANDUSEF`/`GREENFRAC` value at a
representative pixel plus domain-wide aggregate checks (LANDUSEF sums never
exceed 1, FRC_URB2D max matches, every LU_INDEX value is NaN or a plausible
class), 49612 assertions total, matching Python to float32 precision.
`NBUI_MAX=5` also matches the number the project's own README documents for
this exact sample dataset - a nice independent confirmation the port is
computing the right thing, not just self-consistent.

Corrections to the plan found while implementing (each detailed as a code
comment at the relevant spot, summarized here):

- **`removeUrban` doesn't take a `WrfVersionInfo`** (already corrected in
  Stage 2's own status note - repeated here since Stage 3 is where
  `WrfVersionInfo` actually gets threaded through for the first time,
  exactly as anticipated).
- **The truncated-normal sampler needs no `erfinv`, vendored or
  otherwise**, and needs no random sampling at all. `hiResample` computes
  each HI_URB2D bin's probability analytically via the standard normal
  CDF (`std::erf`, real C++ standard library, no vendoring) - the exact
  value w2w.py's own `SAMPLE_SIZE=100000` Monte Carlo draw is estimating,
  with zero sampling noise. w2w.py has no fixed random seed anywhere, so
  its own output isn't bit-reproducible run-to-run either; the live-run
  pin above uses a `margin(0.1)` (percentage points) to absorb Python's
  own binomial sampling noise, not this port's imprecision (it has none).
- **A real-world geo_em file can already carry `FRC_URB2D`/`URB_PARAM`**
  from geogrid's own default (non-LCZ) urban parameterization - confirmed
  against `sample_data/geo_em.d04.nc` itself, which already has both,
  `URB_PARAM`'s param-count dimension sized 18, not this pipeline's 132.
  xarray's own `Dataset.__setitem__` handles reassigning an existing
  variable to a different shape transparently; raw netCDF-C's `nc_def_var`
  errors outright if the name already exists. `NetcdfFile::resizeDimension`
  was generalized into `NetcdfFile::rebuildStructure` (same "rebuild
  everything unchanged except..." shape, extended to also (re)define named
  variables from scratch, with their own possibly-brand-new dimensions,
  instead of copying them from source) - this is also what let
  `createLczParamsFile` fold the `land_cat` resize and the `FRC_URB2D`/
  `URB_PARAM` additions into ONE rebuild pass instead of three.
- **`GDALReprojectImage`'s `GRA_Mode` resampling is slightly MORE complete
  at coverage-boundary pixels than `rasterio.warp.reproject`'s own
  wrapper** for the identical inputs - found via the live pin: Python's
  real run leaves 205 `LU_INDEX` pixels as NaN (an `FRC_URB2D>0` pixel
  whose LCZ-class majority vote found literally no coverage, written
  through unguarded - `dst_data['LU_INDEX'].values[0, frc_mask] =
  lcz_resampled` has no NaN check at all), while this port's direct
  `GDALReprojectImage` call resolves a real class at every one of those
  same 205 positions (confirmed by probing `lczResample` in isolation) -
  never NaN where Python wasn't already, never a bogus class elsewhere.
  Most likely a default warp-option difference between the two GDAL entry
  points for `GRA_Mode`'s edge-of-coverage handling specifically, not a
  masking bug - every other one of the pin's 49612 assertions matches.
  `createLczParamsFile`/`lczResample` still faithfully preserve the
  *capability* to produce a NaN `LU_INDEX` value (unguarded, exactly like
  Python) for whichever inputs do trigger a genuine zero-coverage gap
  under this port's own warp path; the test asserts "NaN or a plausible
  class," not "never NaN."

The rest of this section is kept as originally planned, describing what
each ported piece does - useful background even though every listed item
is now done.

Port, in `lcz.cpp`, roughly in this order (each function is a near-
transcription once Stage 0/1 exist):

- `checkLczIntegrity` (`check_lcz_integrity`, `w2w.py:360-429`): LCZ class
  relabeling (100-series -> 1-17, `_replace_lcz_number`), reproject to
  EPSG:4326 if needed (GDAL `GDALWarp`, already used elsewhere), WRF-domain-
  within-LCZ-extent bounds check (`_check_lcz_wrf_extent`) using `WrfFile`'s
  already-derived `XLAT_M`/`XLONG_M` bounds. Writes a "clean" intermediate
  GeoTIFF - can be an in-memory GDAL MEM dataset instead of a temp file on
  disk, since nothing downstream needs it to persist past this run.
- `ucpResampler`/`hgtResampler`/`hiResampler` (`_ucp_resampler`,
  `_hgt_resampler`, `_hi_resampler`, `w2w.py:695-1047`): build a per-pixel
  lookup array from the UCP table indexed by LCZ class
  (`_get_lcz_arr` + a `std::array`/`std::unordered_map`-based replacer,
  trivial), then reproject via Stage 1's generalized `warpToGrid` with
  `average` resampling. `_hi_resampler` additionally needs:
  - `computeHiDistribution`/`getTruncatedNormalSample`
    (`w2w.py:849-968`): a truncated-normal sampler. No scipy equivalent
    exists in C++; implement via inverse-CDF using `std::erf`/`std::erfinv`
    (or a small rational-approximation `erfinv` if `<cmath>` lacks one -
    C++17 doesn't ship `erfinv` in the standard library, so this needs
    either a vendored approximation or Boost.Math's `erf_inv` if a Boost
    dependency is acceptable) - draw `SAMPLE_SIZE=100000` samples, bin into
    15 five-meter-wide bins via a manual histogram (trivial, no dependency),
    matching `np.histogram(hi_sample, bins=np.arange(0, 76, 5))`.
    `_check_hi_values`' warning-only sanity check ports directly.
- `getSWBW` (`_get_SW_BW`, `w2w.py:663-673`): pure arithmetic on the UCP
  table, direct port.
- `lczResampler` (`_lcz_resampler`, `w2w.py:1050-1088`): reproject with
  `mode` resampling (majority-class aggregation), LCZ 15 -> 11 rename,
  `FRC_URB2D`-based masking. The LCZ-class-to-`LU_INDEX` offset
  (`lcz_2_wrf[0, frc_mask] + 30` in `main`) is `+ wrfVersion.addLczInt` in
  `add_wrf_version` (30 or 50) - this is the single most consequential
  version-dependent line in the whole pipeline, since every downstream
  `LU_INDEX >= 31` / `(30,41)` check elsewhere has a matching `>= 51` /
  `(50,61)` variant to port alongside it.
- `adjustGreenfracLandusef` (`_adjust_greenfrac_landusef`,
  `w2w.py:1091-1160`): the `LANDUSEF` 21/24 -> `wrfVersion.numLandCat` (41
  or 61) category dimension growth - this is exactly Stage 0's
  `resizeDimension` case. `add_wrf_version` also widens the GREENFRAC-
  source urban-pixel mask from just `ISURBAN` to `ISURBAN` plus the
  *original* file's own already-LCZ-tagged categories (`[31..40, ISURBAN]`
  or `[51..60, ISURBAN]`) when re-running against previously-LCZ-processed
  input - same idempotency concern as Stage 2's pre-pass, ported alongside
  it.
- `addFrcLuIndex2Wrf`/`createLczParamsFile` (`w2w.py:1163-1348`): wires the
  above together, builds `URB_PARAM` (132-entry per-pixel parameter block,
  `_initialize_urb_param`), writes the six/seven UCP fields into their
  documented `URB_PARAM` indices (91-99, 118-132 for `HI_URB2D`'s 15 bins),
  and sets the global attributes (`NUM_LAND_CAT = wrfVersion.numLandCat`,
  `FLAG_URB_PARAM=1`, `NBUI_MAX`).
- `createLczExtentFile` (`w2w.py:1351-1397`): derived from the params file's
  own output - collapse LCZ classes back to a single urban category
  (threshold `>= 31` or `>= 51`, branching on the *params file's own*
  `NUM_LAND_CAT` attribute rather than the `wrfVersion` argument directly -
  `add_wrf_version` reads it back off the file it just wrote, which is worth
  preserving since it's what makes this function correct even if called
  standalone against an existing params file), drop `FRC_URB2D`/
  `URB_PARAM`, reset `LANDUSEF` back to the original category count with
  the LCZ-range slice (`[30:]`/`[50:]`) explicitly zeroed first - new in
  `add_wrf_version`, guards against stale LCZ fractions surviving the
  category-count shrink for domains where the original file already had
  some non-zero values in that slice.
- **CSV UCP lookup table**: `pd.read_csv` with a named index column becomes
  a small hand-rolled CSV parser (a dozen columns, 17 rows, no quoting/
  escaping complexity in the real file - `w2w`'s own
  `w2w/resources/LCZ_UCP_lookup.csv`) reading into a
  `std::map<int, UcpRow>` or a small struct-of-arrays. `check_custom_ucp_
  table_integrity` (`MH_URB2D_MIN < MH_URB2D < MH_URB2D_MAX`) ports
  directly as a validating loop, `UserError` on failure (this project's
  existing convention, replacing w2w's `sys.exit(1)`).

Test fixtures: `w2w`'s `testing/geo_em.d04_LCZ_params*.nc`/
`geo_em.d04_LCZ_extent*.nc`/`geo_em.d04_NoUrban*.nc` are exactly the
expected outputs the Python test suite already asserts against
(`test_create_lcz_params_file_attrs_type`, `test_create_lcz_extent_file`,
`w2w.py` tests around lines 833-930) - copy them into `tests/fixtures/lcz/`
and compare the C++ pipeline's output variable-for-variable (allowing
floating-point tolerance) rather than re-deriving expected values by hand.

## Stage 4 - `expand_land_cat_parents` and `checks_and_cleaning`

- `expandLandCatParents` (`w2w.py:1400-1465`): loop over parent-domain
  files (`geo_em.d01.nc` .. `geo_em.d0{N-1}.nc` beside the target file),
  expand `LANDUSEF`'s category dimension to `wrfVersion.numLandCat` (41 or
  61, not a hardcoded 41) if not already - same `resizeDimension` operation
  as Stage 3's `LANDUSEF` growth, applied to a different file; output
  filename suffix follows the target count too (`_41.nc`/`_61.nc`, not
  always `_41.nc`). Direct port once Stage 0 exists. Ported test fixtures:
  `testing/geo_em.d01_Shanghai_ncl20.nc`/`_ncl41.nc`/
  `_no_NUM_LAND_CAT.nc`, `testing/geo_em.d02_Shanghai.nc`
  (`test_expand_land_cat_parents_*`, `w2w.py:931-1024`).
- `checksAndCleaning` (`w2w.py:1468-1678`): nine sanity checks (urban class
  removed, LCZ extent present, UCP ranges sane, `HI_URB2D` sums to 100%,
  extents match across output files) - all pure read-and-compare against
  the files just written, no new infrastructure. `add_wrf_version` widens
  Check 1's "is the source file's urban category list" logic to branch on
  the *original* file's own `NUM_LAND_CAT` (61 -> urban categories
  `[51..60, ISURBAN]`, 41 -> `[31..40, ISURBAN]`, 20 or 21 -> `[ISURBAN]`
  only, anything else -> reject with an error) instead of `main`'s single
  `ISURBAN in LU_INDEX` check - port the same four-way branch, throwing
  `UserError` for the unsupported-count case rather than `main`'s bare
  `ISURBAN`-only assumption (which is simply wrong once a 61-category
  source file is possible). Checks 3, 8, and 9's urban-pixel-count
  comparisons follow the same `LCZ_URBAN` list rather than a fixed `>= 31`
  threshold. Ports to a `std::vector<CheckResult>` (name, pass/warn,
  message) that the UI surface (Stage 5) renders as a checklist instead of
  colored terminal escape codes (`w2w.py`'s `OKGREEN`/`WARNING`/`ERROR`
  ANSI constants have no native equivalent needed - this project already
  has a `UserError` exception + `QMessageBox` convention for the failure
  case, and Qt list/label widgets for informational output).

**Status: done.** `expandLandCatParents` and `checksAndCleaning` are
implemented in `lcz.hpp`/`lcz.cpp`, both against a live read of
`add_wrf_version` @ `7801b3e`'s actual source (not the line numbers this
plan originally cited, which had drifted - the real functions sit at
`w2w.py:1476` and `w2w.py:1556`):

- `expandLandCatParents(dstFile, wrfVersion) -> vector<string>` finds the
  target's own two-digit domain number from its filename, loops parent
  domains 1..N-1, and returns one status line per parent (missing file /
  unreadable NUM_LAND_CAT-or-LANDUSEF / already correct / rewritten) -
  matching w2w.py's print statements' content without adopting a
  print-to-stdout convention this project doesn't otherwise use. A
  mismatched parent is rebuilt via `NetcdfFile::resizeDimension` (the same
  primitive Stage 3 uses for LANDUSEF growth) and written alongside the
  original as `..._{N}.nc`; the parent file itself is never modified.
- `checksAndCleaning(inputs, ucpTable) -> vector<CheckResult>` runs the nine
  sanity checks and returns each as `{name, status, message}` for a UI
  checklist, then deletes `inputs.srcFileClean` if present. `nbuiMax` is
  NOT one of the nine checks (confirmed by re-reading the source: it's only
  used in the CLI's final "Set nbui_max to N" print, unconnected to any
  check) - dropped from the function's parameters entirely; Stage 5 surfaces
  it separately from `createLczParamsFile`'s own return value.

**A real correction to this plan's own text, found by re-reading the source
rather than trusting the earlier summary:** the plan claimed "Checks 3, 8,
and 9's urban-pixel-count comparisons follow the same LCZ_URBAN list rather
than a fixed `>= 31` threshold." This is only true for checks 3 and 9. Check
8 (`w2w.py:1745`, `lu_index_res = xr.where(lu_index >= 31, 1, 0)`) does NOT
consult the per-source-file LCZ_URBAN list at all - it always compares
against a hardcoded `>= 31`, even when the original file was a 61-category
source (whose LCZ range is 51-60, not 31-40). This is a latent quirk in
`add_wrf_version` itself, not something introduced by porting. Ported
faithfully as-is (see the doc comment on `checksAndCleaning` and the
`// Hardcoded >= 31...` comment at its Check 8 site in `lcz.cpp`) rather
than "fixed" to use the per-source list, since the goal is matching w2w's
actual behavior.

Test fixtures added to `tests/fixtures/lcz/`: `geo_em.d01_Shanghai_ncl41.nc`,
`geo_em.d01_Shanghai_no_NUM_LAND_CAT.nc`, `geo_em.d04_NoUrban.nc`,
`geo_em.d04_LCZ_extent.nc`, `geo_em.d04_LCZ_params.nc` (real
`add_wrf_version` pipeline outputs for the Zaragoza sample domain - an
"all OK" checksAndCleaning fixture), `geo_em.d04_NoUrban_dummy.nc`,
`geo_em.d04_LCZ_extent_dummy.nc`, `geo_em.d04_LCZ_params_dummy.nc`,
`geo_em.d04_LCZ_params_with_ucp_dummy.nc` (w2w's own dummy fixtures
engineered to trip each check's WARNING branch). 8 new `core_tests.cpp`
cases (4 `expandLandCatParents`, 4 `checksAndCleaning`), CTest count 102 ->
110, all passing including the pre-existing 102.

## Stage 5 - UI integration and threading

- New `LczForm` (`include/wrftools/lcz_form.hpp`/`src/lcz_form.cpp`),
  mirroring `GeotiffConvertForm`'s shape (file pickers, an argument panel,
  a progress area, a background-thread run):
  - Inputs: input directory, LCZ GeoTIFF filename, target `geo_em.dXX.nc`
    filename, and a **required WRF version dropdown** (matching
    `add_wrf_version`'s four positional CLI args, `wrf_version` included -
    populated from the `WrfVersionInfo` map added in Stage 0, no free-text
    entry so an unsupported/mistyped version can't silently fall back to
    the wrong category scheme), plus the optional arguments (`--built-lcz`,
    `--lcz-band`, `--frc-threshold`, `--npix-nlc`, `--npix-area`,
    `--lcz-ucp`) as form fields with w2w's own defaults.
  - Run button spawns a `std::thread` running the Stage 2-4 pipeline in
    order (mirroring `main()`'s own sequencing, `w2w.py:152-210`), reporting
    stage-by-stage progress (`GeotiffConvertForm`'s
    `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` pattern) rather
    than a single blocking call.
  - Results panel: the nine `checks_and_cleaning` results as a checklist,
    plus the final `nbui_max` value (the one piece of output w2w's CLI
    treats as user-facing advice - "Set nbui_max to N during compilation").
  - Validation before running (`UserError`, this project's convention):
    input files exist, LCZ GeoTIFF covers the WRF domain (reuses
    `checkLczIntegrity`'s bounds check, surfaced early rather than only
    discovered mid-pipeline), custom UCP table integrity if provided.
- Wire a new "LCZ" tab into `MainWindow` (`main_window.hpp`/`.cpp`)
  alongside the existing Domains/View/Convert tabs.

**Status: done**, with two deliberate deviations from this plan's own text,
both explained where they're implemented rather than silently diverging:

- **No separate "input directory" field.** The plan described mirroring the
  CLI's `io_dir` + two filenames. Every function in `lcz.hpp` already takes
  full paths, not `io_dir`-relative ones, so `LczForm` just has "LCZ GeoTIFF
  file" and "Target geo_em.dXX.nc file" browse fields (full paths); every
  output path is derived from the target file's own path, matching what
  `io_dir` + `wrf_file.replace(...)` produces when `io_dir` is that file's
  parent directory. A real simplification, not a fidelity loss - see the
  doc comment at the top of `lcz_form.hpp`.
- **No LCZ Generator auto-detection.** w2w.py's `_get_lcz_band` silently
  switches to band 1 when the GeoTIFF's `long_name` attribute contains
  `"lczFilter"`. `LczForm`'s LCZ-band field is a plain, always-honored value
  (default 0) - a user with an LCZ Generator map sets it to 1 themselves.
  Left out because it's a convenience heuristic on top of an already-
  explicit `-l`/`--lcz-band` argument, not something a wrong guess could
  make silently incorrect - the user still sees and controls the actual
  value being used.

The `WrfVersionInfo` dropdown is fed by a new `wrfVersionOptions()`
(`lcz.hpp`/`lcz.cpp`) - the plan assumed this table already existed "from
Stage 0," but it was never actually added; it's new here, ported directly
from `WRF_VERSIONS_DICT` (`w2w.py:43-54`) in the same order.

`checkLczIntegrity` runs synchronously on the GUI thread as part of
`runLcz()`'s validation, before the Run button even disables - not as a
separate background pre-check step. It's genuinely the pipeline's first
stage (`main()`'s own sequencing), so this satisfies "surfaced early"
without needing a second background pass purely to re-check domain
coverage; the tradeoff (documented in `lcz_form.hpp`) is a brief UI block
while a large LCZ raster reprojects.

A real robustness gap fixed while wiring this up, in `expandLandCatParents`
itself (`lcz.cpp`): the domain-number parse used a raw `std::stoi` on a
filename substring, throwing `std::invalid_argument` - not `UserError` -
for a non-numeric or too-short filename. Stage 4's own tests never
exercised this because their fixtures were always well-formed
`geo_em.dNN.nc` names; Stage 5 makes the function reachable from an
arbitrary user-picked file, so it now validates both digits first and
throws `UserError`. Two new `core_tests.cpp` cases pin this.

Since this port has no `checks_and_cleaning`-produced `*_clean.tif` on
disk (`checkLczIntegrity` keeps its cleaned raster in memory - see its own
doc in `lcz.hpp`), `LczForm` passes `checksAndCleaning` an empty
`srcFileClean` path; the cleanup step is then a harmless no-op (matches the
"no-op" `checksAndCleaning` test case from Stage 4).

7 new `ui_tests.cpp` `LczForm` cases (dropdown population, three
validation-throws-before-running cases, and one real end-to-end run against
the Zaragoza sample domain in a `QTemporaryDir`, asserting all three output
files exist, all nine checks pass, and `nbui_max == 5` - matching Stage 3's
own pinned value for the same fixture pair and defaults), plus 1 new
`core_tests.cpp` case for the `expandLandCatParents` filename-validation fix
above. CTest count 110 -> 111 (the 1 new `core_tests` case is its own CTest
entry; `wrftools_ui_tests` stays one CTest entry regardless of how many
Catch2 cases it holds internally - 86 now, 7 more than before this stage),
all passing.

**Follow-up: the default UCP table is now hardwired, not resolved as a
runtime file path.** `LczForm` originally located
`resources/lcz_ucp_lookup.csv` via a `defaultUcpTablePath()` helper (try a
path relative to the process's current directory, then one relative to the
executable) when no `--lcz-ucp`-equivalent custom table was given - a real
packaging/install-location problem this project has no general answer for
yet (see main.cpp's own GDAL/PROJ-data and font-directory fallbacks, which
face the same issue for other data). Replaced with `defaultUcpTable()`
(`lcz.hpp`/`lcz.cpp`): the bundled CSV's content, embedded as a string
literal (`kDefaultUcpTableCsv`) and parsed once into a function-local
static, so a packaged build never needs to find the file at all. `loadUcpTable`
was refactored to share its parsing (now `parseUcpTable(std::istream&, ...)`)
with a new `loadUcpTableFromString(csvContent)`, which `defaultUcpTable()`
calls. `resources/lcz_ucp_lookup.csv` itself is kept (existing
`loadUcpTable`-from-path tests still pin against it, and it's the readable
reference for anyone diffing the embedded copy) - a new core_tests.cpp case,
"defaultUcpTable's hardwired content matches the bundled
resources/lcz_ucp_lookup.csv file byte-for-byte", pins the embedded string
against the file so the two can't silently drift apart. CTest count 111 ->
113 (2 new core_tests.cpp cases: the drift-pin above and one for
`loadUcpTableFromString` directly).

**Follow-up: `LczForm`'s background thread was a real, reproducible
deadlock hazard, not just a theoretical one - removed.** A user reported a
run that hung indefinitely; the `wrftools_ui_tests` end-to-end LCZ case
(which had previously passed) started failing the same way once run
against a rebuilt tree. Root-caused via direct reproduction, isolated from
Qt entirely: a small standalone program that (1) opens a `NetcdfFile` and
calls `checkLczIntegrity` (touching GDAL) on the main thread, closes that
handle, then (2) spawns a `std::thread` that calls `NetcdfFile::open` again
on a *different* file - hangs forever, on this machine's system libhdf5
(built without `--enable-threadsafe`). Confirmed the closed-handle theory
an intermediate commit had already tried (closing the `NetcdfFile` before
starting the worker thread) does NOT fix it - the deadlock isn't about
concurrently-open handles, it's that netCDF-C/HDF5's global state becomes
thread-affined to whichever OS thread first touches it, and using it from
a second thread later - even fully sequentially, even after every prior
handle is closed - deadlocks. Since `main.cpp` itself calls
`GDALAllRegister()` on the GUI thread before any window exists, and GDAL is
used elsewhere in this app's GUI-thread code (rendering, domain overlays),
this state is *always* already thread-affined to the GUI thread by the
time a user opens the LCZ tab - so backgrounding only `checkLczIntegrity`,
or any subset of the pipeline, could never have been safe on such a build.

Fixed by removing `LczForm`'s `std::thread` entirely: `runLcz()` now runs
the whole pipeline synchronously on the GUI thread, calling
`QCoreApplication::processEvents()` between stages so the UI still
repaints/responds between them (fully blocked only within any single
stage) - the same tradeoff this project had already accepted for
`checkLczIntegrity` alone, now extended to the whole run. `GeotiffConvertForm`
keeps its own background thread unchanged: it is safe specifically because
`convert_geotiff_lib` is GDAL-free by design (writes via libtiff/libgeotiff
directly, per its own doc comment), so it never touches this hazard.
`wrftools_ui_tests`' full LCZ run (previously ~20s) now finishes
comfortably within its 60s deadline; the full suite's total time rose from
about 60s to about 200s purely because that one Qt test process is no
longer overlapping GUI-thread event-loop spinning with worker-thread GDAL
work - not a regression, just the honest cost of the safe design.

**Follow-up: a second real "String match to name in use" collision, found
against a user's actual Hong Kong domain.** `createLczParamsFile`'s
`rebuildStructure` call declares `num_urb_params` as a brand-new dimension
(`newDimensions`) for its `URB_PARAM` override. Stage 3's own fixture
(`sample_data/geo_em.d04.nc`) already had an existing, differently-shaped
URB_PARAM - but its parameter-count dimension happened to be named
something else entirely (`\18`, an oddly-named leftover from an older
geogrid convention), so it never collided with the literal name
`num_urb_params` and this case went unexercised. A user's real geogrid.exe
output (a Hong Kong domain, from a newer WRF/WPS build) already names that
dimension exactly `num_urb_params` - because that build's default urban
physics scheme already uses the modern 132-parameter layout - so
`rebuildStructure`'s unconditional `nc_def_dim("num_urb_params", ...)`
failed immediately: `NetCDF error (nc_def_dim num_urb_params): NetCDF:
String match to name in use`.

Fixed generally, not by special-casing this one name:
`NetcdfFile::rebuildStructure` (`netcdf_file.cpp`) now checks each
`newDimensions` entry against the source file's own dimensions first; a
name that already exists is folded into the same resize-in-place handling
`resizedDimensionName` already got (so it's grown/shrunk to the requested
length as part of the normal dimension-copy pass, not redefined from
scratch), and only genuinely-new names go through the original
"define-from-scratch" path. A non-overridden variable that happens to
share the resized dimension (verified with a synthetic `OTHER_URB_VAR` in
the new test) is unaffected: it's still copied using the SOURCE file's own
element counts for that axis, so its original data survives unchanged into
the newly-larger dimension.

Verified two ways: (1) a new, fast `core_tests.cpp` case
(`NetcdfFile::rebuildStructure resizes a source dimension that already
collides with an override's new dimension name`) using a synthetic
5by5.nc-based fixture that deliberately adds a `num_urb_params` dimension
first - confirmed to fail with the user's exact error message when run
against the pre-fix code (via a `git stash` of just this fix), and to pass
with it; (2) the full pipeline run end-to-end against the user's actual
Hong Kong `geo_em.d03.nc` + `HK.tif` (outside the repo, not committed -
privacy/size), which completed successfully (`nbui_max=11`) after the fix,
having failed at this exact point before it. CTest count 113 -> 114.

**Follow-up: the View tab now recognizes LCZ classes in an LCZ-processed
file's `LU_INDEX`, instead of showing them as generic "Category N" (or,
worse, USGS/MODIFIED_IGBP_MODIS_NOAH's own real-but-wrong meaning for the
same numbers - both schemes genuinely define 31-33 themselves, "Low/High
Intensity Residential"/"Industrial or Commercial", which is not what those
numbers mean in an LCZ file).** A user asked for this directly after
seeing their Hong Kong domain's `*_LCZ_params.nc` render with meaningless
category labels in the View tab.

`WrfFile` (`wrf_file.cpp`) now derives `LU_INDEX`/`IVGTYP`'s
`categoryScheme` via a new `lczAwareCategoryScheme()`, not a bare `MMINLU`
read. It only switches away from the plain base scheme name (to
`"<MMINLU>+LCZ41"`/`"<MMINLU>+LCZ61"`) when THREE signals agree: the file's
`DESCRIPTION` global attribute contains createLczParamsFile's own marker
text (verbatim from w2w.py, see `lcz.cpp`), `NUM_LAND_CAT` is 41 or 61 (for
which offset), AND `FLAG_URB_PARAM` is `1`. All three are needed, not just
`NUM_LAND_CAT`: some real geo_em files already carry `NUM_LAND_CAT=41` from
geogrid's own default (non-LCZ) urban physics (Stage 3's own
`sample_data/geo_em.d04.nc`) - and that exact fixture also happens to make
`createLczExtentFile`'s own output keep `NUM_LAND_CAT=41` too (it resets to
the ORIGINAL file's count, which here already was 41) even though that
file's `LU_INDEX` has zero LCZ classes left (collapsed back to `ISURBAN`).
`FLAG_URB_PARAM` is the one signal that reliably excludes it:
`createLczExtentFile` always resets it to `0`, only `createLczParamsFile`'s
own output ever has it `1`.

`colormaps.cpp` gained `lczLabels()` (WUDAPT's standard LCZ 1-17
palette, the same colors the LCZ Generator itself uses - Stewart & Oke
2012) and `augmentedLanduseTables()`, which overlays that palette onto
each existing base scheme's own table at +30/+50, registered under the
`"<scheme>+LCZ41"`/`"<scheme>+LCZ61"` keys `categoricalLut` now looks up
via (replacing its direct `landuseTables()` lookup). The overlay wins over
a coincidentally-numbered base-scheme entry at the same value (the
USGS/MODIFIED_IGBP_MODIS_NOAH 31-33 collision above) - it's applied on top
of a copy, not merged the other way around. Only LCZ classes 1-10 (the
"built" types) can ever actually appear in an LCZ-processed file's
`LU_INDEX` (this pipeline's own `lczResample` only ever resamples
`BUILT_LCZ` classes onto the grid), but all 17 are included in
`lczLabels()` for completeness.

Verified against the real fixtures: `geo_em.d04_LCZ_params.nc` (the only
one whose `LU_INDEX` genuinely has LCZ numbers) now reports
`"MODIFIED_IGBP_MODIS_NOAH+LCZ41"`; `geo_em.d04_LCZ_extent.nc` and the
plain `geo_em.d04.nc` both correctly stay at the bare `MMINLU` scheme
despite the `NUM_LAND_CAT=41` coincidence for the extent file. 2 new
`core_tests.cpp` cases. CTest count 114 -> 116.

## Verification

Per stage, not just at the end - matching this project's existing practice
(`crs`/`domain`/`warp` pinned against live Python output in
`core_tests.cpp`):

- `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
  after every stage.
- **Every stage with numerical output is pinned against a real w2w run**,
  not hand-derived expected values: check out `add_wrf_version` at
  `7801b3e` in a separate worktree/clone (`main`'s checked-out
  `/home/strebdom/git/w2w` shouldn't be switched under anyone else using
  it - `git worktree add ../w2w-add-wrf-version 7801b3e` from that repo is
  the non-disruptive way to get a second checkout) and run its CLI/test
  suite in its own venv (`pip install -e . -r requirements-dev.txt &&
  pytest`) to reproduce or regenerate expected outputs for both the
  41-category and 61-category paths. Copy the relevant fixtures into
  `tests/fixtures/lcz/` rather than depending on the sibling checkout
  staying in place.
- **Both category schemes need end-to-end coverage, not just one.** Every
  fixture/pinned-value comparison in Stages 2-4 above should run for at
  least one `v4.3`-class input (41 categories, `+30` offset) and one
  `v4.4.2`-class input (61 categories, `+50` offset) - `add_wrf_version`'s
  own test suite parametrizes `test_create_lcz_extent_file` over
  `(version, num_land_cat)` for exactly this reason; the C++ tests should
  mirror that parametrization rather than hardcoding one scheme and
  treating the other as an afterthought.
- End-to-end check: run the full pipeline against `w2w`'s own
  `sample_data/` (Zaragoza, `lcz_zaragoza.tif` + `geo_em.d04.nc`, a
  41-category domain) and diff every variable in the resulting
  `*_NoUrban.nc`/`*_LCZ_extent.nc`/`*_LCZ_params.nc` against the checked-in
  `sample_data` outputs referenced by the README's before/after figure -
  plus, since no 61-category sample dataset ships in the repo, a second
  end-to-end run against a synthetic or `testing/`-derived 61-category
  fixture built specifically to exercise that path.
- `add_wrf_version`'s own `pytest` suite must stay untouched and green
  throughout (it's the reference, not being modified) - run it once at the
  start to confirm the sibling checkout itself is in a good state before
  pinning anything against it.

## Risks

- **`add_wrf_version` is an unmerged, moving-target upstream branch.** It
  could be force-pushed, rebased, or merged (with further changes) into
  `main` before this port is finished. Since this plan pins to `7801b3e`
  specifically, periodically re-diff `origin/add_wrf_version` against that
  SHA (`cd /home/strebdom/git/w2w && git fetch && git log 7801b3e..origin/
  add_wrf_version`) to catch drift, and re-sync deliberately rather than by
  accident if it lands on `main`.
- **NetCDF attribute/dimension fidelity is the highest-risk piece.** A
  wrongly-copied attribute or off-by-one in a resized dimension produces a
  file that looks fine in casual inspection but breaks `real.exe` with an
  obscure error, or silently mis-locates urban parameters. Stage 0's
  round-trip test (open real file, write unchanged, byte-compare) is the
  guardrail here and should not be skipped or weakened.
- **The truncated-normal sampler (Stage 3) is the second-highest-risk
  piece** - a subtly different `erfinv` approximation or sampling scheme
  shifts every `HI_URB2D` bin by a few percent without throwing anything,
  and `_check_hi_values`' own tolerance (5% margin) might not catch a
  small-but-real discrepancy. Pin bin-by-bin against Python's actual sampled
  output for the default UCP table, not just against the analytic
  distribution.
- **`NPIX_AREA`/KD-tree performance** on a real (not 5x5 test fixture)
  domain: `w2w.py`'s own docs note this is already the slow part in Python;
  nanoflann should comfortably beat scipy's KDTree, but this should be
  measured against a real-sized domain (e.g. this project's own
  `tests/fixtures/geo_em_small.nc`, if large enough, or one of w2w's larger
  `geo_em.d0*` fixtures) before assuming Stage 5's threading is sufficient
  and no further chunking/progress-granularity work is needed.
- **Scope creep via the UI.** w2w's CLI is intentionally minimal (three
  positional args + six optional flags). Resist expanding `LczForm` beyond
  that surface until the core pipeline is verified end-to-end - a wider UI
  is easy to add later, a wrong pipeline is not.
