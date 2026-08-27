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

- **Generalize `warpToWebMercator`** (`warp.hpp`/`warp.cpp`) into a
  `warpToGrid(values, width, height, sourceWkt, sourceGeotransform, destWkt,
  destGeotransform, destWidth, destHeight, resampling)` that takes an
  explicit destination CRS/transform/shape/resampling method instead of
  hardcoding EPSG:3857 + bilinear. Keep `warpToWebMercator` as a thin
  wrapper calling the generalized function with EPSG:3857 + bilinear, so
  every existing caller (`LayerRenderer`, `WpsBinarySource`) and its tests
  are untouched. `Resampling` needs `average` and `mode` (`rasterio.warp.
  Resampling.average`/`.mode` -> `GDALWarpOptions::eResampleAlg =
  GRA_Average`/`GRA_Mode`, both already valid GDAL enum values, no new
  dependency).
- **WRF-grid transform from a geo_em file**, porting `_get_wrf_grid_info`
  (`w2w.py:581-660`) - this is *not* the same geotransform `WrfFile`
  already derives from the staggered U/V grids (`wrf_file.cpp`); w2w derives
  it from `DX`/`DY`/`CEN_LON`/`CEN_LAT`/grid size directly, centered on the
  domain's projected center point, matching `Affine.translation(x0 - dx/2,
  y0 - dy/2) * Affine.scale(dx, dy)` exactly. Reuse `Crs::lambert`/
  `Crs::polar`/`Crs::mercator`/`Crs::lonLat` (already ported, already pinned
  against Python) for the four `MAP_PROJ` branches - `w2w.py`'s own proj4
  construction (`a=6370000, b=6370000`, `lat_1/lat_2/lat_0/lon_0` for
  Lambert, `lat_ts` for Mercator/Polar) already matches `crs.cpp`'s, so this
  should be a near-transcription, not new projection math.
- Pin both against the Python reference the same way `core_tests.cpp`
  already does for `crs.cpp`/`warp.cpp`: run `w2w`'s own
  `_get_wrf_grid_info`/`reproject` against `testing/5by5.nc` (or one of the
  `geo_em.d0*_Shanghai*.nc` fixtures) via `uv run python -c ...` (a venv
  with w2w's dependencies installed, separately from wrftools), compare the
  6-term transform and a handful of warped-raster sample values.

## Stage 2 - `wrf_remove_urban` (`*_NoUrban.nc`)

Port `wrf_remove_urban` per `add_wrf_version`'s version
(`w2w.py:456-576` there, not `main`'s) as `include/wrftools/lcz.hpp` /
`src/lcz.cpp`, `removeUrban(NetcdfFile& dst, const WrfVersionInfo& wrfVersion,
int npixNlc, std::optional<int> npixArea)`:

- **Pre-pass: collapse any existing LCZ categories back to urban**, new in
  `add_wrf_version` (not in `main`): if the input file's `NUM_LAND_CAT` is
  already 41 or 61 (i.e. this is a re-run against a file `w2w` itself
  already produced), `LU_INDEX` values in `(30,41)`/`(50,61)` are folded
  back to `ISURBAN` and their `LANDUSEF` fractions summed into the urban
  category before the normal removal loop runs, so the tool is idempotent
  on its own output. Port this as an explicit branch on `wrfVersion.
  numLandCat` matching `orig_num_land_cat`, mirroring the two `elif`
  branches added there.
- The `NoUrban` output's `NUM_LAND_CAT` global attribute is explicitly reset
  to 21 (`dst_data.attrs['NUM_LAND_CAT'] = np.intc(21)`), new in
  `add_wrf_version` - `main` leaves whatever the source file already had.
- **KD-tree**: vendor [nanoflann](https://github.com/jlblancoc/nanoflann)
  (single header, MIT license, no build-system integration needed beyond
  adding the header to `include/third_party/` or fetching via CMake
  `FetchContent`) for the ECEF-chord nearest-neighbor search
  (`using_kdtree`, `w2w.py:432-453`: `R=6371`, spherical-to-Cartesian, query
  k nearest). A hand-rolled KD-tree is not worth the risk of a subtle bug
  here - this is exactly nanoflann's designed use case.
- Straight port of the per-urban-pixel loop: for each urban pixel, take its
  `NPIX_AREA` nearest neighbors, filter to natural-land pixels, keep the
  first `NPIX_NLC`, take the modal `LU_INDEX` among those, and average
  `GREENFRAC` over pixels sharing that mode. `pandas`' `.mode()` becomes a
  simple `std::unordered_map<int,int>` tally with first-seen tiebreak
  (matches pandas' stable-sort mode behavior for the tie case, which the
  Python test suite doesn't appear to exercise directly but is worth
  matching for determinism).
- `LANDUSEF`/`GREENFRAC` reassignment (`w2w.py:558-571`) is direct array
  arithmetic - no new infrastructure once Stage 0's read/write is in place.
- Run this on a background thread (see Stage 5) since this is the slowest
  single step in the pipeline for a real domain.
- Test against `w2w`'s `testing/5by5.nc`/`testing/5by5_usgs.nc`/
  `testing/5by5_20cat.nc` fixtures (small enough to assert on every pixel)
  and pin against `test_wrf_remove_urban` (`tests/w2w_test.py:346-375`)'s
  expected values.

## Stage 3 - `create_lcz_params_file` / `create_lcz_extent_file` (the core UCP pipeline)

The largest single piece. Port, in `lcz.cpp`, roughly in this order (each
function is a near-transcription once Stage 0/1 exist):

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
