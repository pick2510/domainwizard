#pragma once

#include "wrftools/crs.hpp"
#include "wrftools/netcdf_file.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wrftools {

// add_wrf_version's WRF_VERSIONS_DICT: which LU_INDEX offset and target
// land-category count a given WRF release expects for LCZ classes
// (v4.3-v4.4.1: +30/41 categories; v4.4.2-v4.5.2: +50/61 categories).
struct WrfVersionInfo {
    int addLczInt{};
    int numLandCat{};
};

// One entry of add_wrf_version's WRF_VERSIONS_DICT (w2w.py:43-54), in its
// own declared order - v4.3 through v4.4.1 use the 41-category scheme,
// v4.4.2 through v4.5.2 use 61. This is the dropdown Stage 5's LczForm
// populates: a fixed, non-free-text choice, matching the CLI's own
// `choices=WRF_VERSIONS_DICT.keys()` (an unrecognized/mistyped version
// string has no way to silently fall back to the wrong category scheme).
struct WrfVersionOption {
    std::string name;
    WrfVersionInfo info;
};
[[nodiscard]] const std::vector<WrfVersionOption>& wrfVersionOptions();

// One row of w2w's LCZ_UCP_lookup.csv (or a user-supplied replacement via
// --lcz-ucp) - the Stewart & Oke-derived urban canopy parameters per LCZ
// class (1-17).
struct UcpRow {
    double frcUrb2d{};
    double mhUrb2dMin{};
    double mhUrb2d{};
    double mhUrb2dMax{};
    double bldfrUrb2d{};
    double h2w{};
};

// Parses a UCP lookup CSV (w2w's own format: an unnamed index column of
// LCZ class numbers 1-17, then FRC_URB2D/MH_URB2D_MIN/MH_URB2D/
// MH_URB2D_MAX/BLDFR_URB2D/H2W columns, in any order - column names are
// matched by name, not position, and both header and value fields are
// whitespace-trimmed, matching w2w's own `.rename(columns=lambda x:
// x.strip())` and pandas' own numeric-parsing whitespace tolerance).
// Throws UserError on a malformed file.
[[nodiscard]] std::map<int, UcpRow> loadUcpTable(const std::filesystem::path& csvPath);

// Same parsing as loadUcpTable, from an already-in-memory CSV string rather
// than a file path - what defaultUcpTable() (below) uses to parse its
// hardwired content, and available directly for anything else that has the
// content in memory already.
[[nodiscard]] std::map<int, UcpRow> loadUcpTableFromString(const std::string& csvContent);

// w2w's own bundled resources/LCZ_UCP_lookup.csv, hardwired into the
// binary (parsed once from a string literal matching
// resources/lcz_ucp_lookup.csv byte-for-byte - see the
// "defaultUcpTable() matches..." core_tests.cpp case that pins the two
// together) rather than resolved as a runtime file path relative to the
// process's current directory or the executable. Used whenever no custom
// --lcz-ucp table is supplied - avoids a packaging/install-location problem
// this project doesn't otherwise have a general answer for (see
// PORT_W2W.MD Stage 5's original defaultUcpTablePath(), now removed).
[[nodiscard]] const std::map<int, UcpRow>& defaultUcpTable();

// Ports check_custom_ucp_table_integrity: MH_URB2D_MIN must be < MH_URB2D
// must be < MH_URB2D_MAX for every BUILT LCZ class (1-10 - w2w.py only
// ever checks these, not the full 1-17 table, since 11-17 are non-BUILT
// classes with all-zero UCP rows by convention). Throws UserError,
// matching this project's convention (w2w.py exits the process).
void checkCustomUcpTableIntegrity(const std::map<int, UcpRow>& ucpTable);

// The WRF grid a geo_em file defines, as a CRS + a GDAL-style 6-term
// geotransform - ported from w2w's own _get_wrf_grid_info
// (w2w/w2w.py:581-660 on the add_wrf_version branch, see PORT_W2W.MD Stage
// 1), NOT the same geotransform WrfFile derives elsewhere in this project.
// Two real differences from WrfFile's own geotransform:
//   - WrfFile reads through GDAL's NetCDF driver, which silently flips
//     rows to present a north-up (top-down) array; NetcdfFile (used here)
//     reads the raw on-disk order directly, matching what w2w's own
//     xr.open_dataset sees - WRF's own south_north dimension increases
//     NORTHWARD (row 0 is the southernmost row), so the geotransform below
//     has a POSITIVE row-height term, not GDAL's usual negative one. Any
//     data warped through this geotransform (see warpToGrid) must come
//     from the same raw, un-flipped row order - never from WrfFile.
//   - The projected origin (e, n) is computed by transforming the file's
//     own CEN_LON/CEN_LAT through REAL WGS84 (Crs::wgs84()), not through
//     this project's usual no-datum-shift WRF-sphere lon/lat variant
//     (Crs::toXy) - matching w2w's own pyproj.Proj(proj='latlong',
//     datum='WGS84') source CRS exactly, since that's what values are
//     pinned against.
struct WrfGridInfo {
    Crs crs;
    std::array<double, 6> geotransform{};
    int width{};   // west_east
    int height{};  // south_north
};

[[nodiscard]] WrfGridInfo wrfGridInfo(const NetcdfFile& file);

// Ports w2w's wrf_remove_urban (w2w.py, add_wrf_version branch - see
// PORT_W2W.MD Stage 2): replaces every urban LU_INDEX pixel with the
// dominant land-use category among its nearest natural-land neighbors
// (great-circle nearest-neighbor search, matching using_kdtree exactly -
// R=6371 km ECEF projection), moves each pixel's urban LANDUSEF fraction
// into the resulting category, and averages GREENFRAC over the neighbors
// sharing that category. Also collapses any LCZ-range LU_INDEX values the
// SOURCE file may already carry (orig NUM_LAND_CAT 41 or 61) back to
// ISURBAN first, so re-running this against a file w2w itself already
// produced is idempotent - new in add_wrf_version, absent from w2w's
// `main`.
//
// `srcPath` is read but never modified; the result (a full copy of
// `srcPath` with LU_INDEX/LANDUSEF/GREENFRAC replaced and NUM_LAND_CAT set
// to 21) is written to `dstPath`, overwriting it if present - mirrors
// wrf_remove_urban always regenerating info.dst_nu_file from info.dst_file
// each run, not modifying dst_file in place.
//
// npixArea defaults to npixNlc*npixNlc when absent, matching
// args.NPIX_AREA's own default. Throws UserError if npixArea exceeds the
// domain's pixel count, or if any urban pixel has zero natural-land
// neighbors within its npixArea-nearest set (both raise-and-exit in
// Python; this project's convention is to throw instead).
void removeUrban(const std::filesystem::path& srcPath, const std::filesystem::path& dstPath, int npixNlc, std::optional<int> npixArea = std::nullopt);

// An LCZ raster normalized to EPSG:4326 - w2w's own "_clean.tif"
// (check_lcz_integrity, w2w.py:360-429), kept in memory rather than
// written to a temp file since nothing downstream needs it to persist.
struct LczRaster {
    std::vector<float> values;  // class labels 1-17, row-major top-down
    int width{};
    int height{};
    std::string wkt;  // always a canonical EPSG:4326 WKT, regardless of the source file's own CRS
    std::array<double, 6> geotransform{};
};

// Ports check_lcz_integrity: relabels a 100-series (LCZ Generator) class
// scheme to 1-17, reprojects to EPSG:4326 if the source isn't already
// (nearest-neighbor, since classes are categorical), and verifies the LCZ
// raster's extent covers `wrfFile`'s domain (its XLONG_M/XLAT_M bounds) in
// every direction - throws UserError if it doesn't, matching Python's
// sys.exit(1) via this project's own convention. `lczBand` is 0-indexed,
// matching w2w's own `-l`/`--lcz-band` argument.
[[nodiscard]] LczRaster checkLczIntegrity(const std::filesystem::path& lczPath, int lczBand, const NetcdfFile& wrfFile);

// The six UCP fields resampled through _ucp_resampler in
// create_lcz_params_file's loop (w2w.py, add_wrf_version) - an enum
// instead of a string key, since these are the only ucp_key values ever
// actually passed (HGT_URB2D and HI_URB2D go through their own dedicated
// resamplers below, not this one).
enum class UcpKey { FrcUrb2d, MhUrb2d, StdhUrb2d, LbUrb2d, LfUrb2d, LpUrb2d };

// Ports _ucp_resampler: builds a per-pixel lookup value from `ucpTable`
// for each class in `builtLcz` (LB/LF/LP_URB2D use the Zonato et al. 2020
// lambda formulas via getStreetWidth/getBuildingWidth below; STDH_URB2D is
// (MAX-MIN)/4; everything else is a direct column lookup), masks every
// other class to 0, and reprojects onto the WRF grid with AVERAGE
// resampling. `frcThreshold`, when set, zeroes any resampled value at or
// below it (only ever passed for `FrcUrb2d`). NaN (nothing covered a
// destination pixel) becomes 0.
[[nodiscard]] std::vector<float> ucpResample(const LczRaster& clean, const WrfGridInfo& grid, const std::vector<int>& builtLcz,
    const std::map<int, UcpRow>& ucpTable, UcpKey key, std::optional<double> frcThreshold = std::nullopt);

// Street Width / Building Width per LCZ class (_get_SW_BW): SW = MH_URB2D
// / H2W; BW = BLDFR_URB2D / (FRC_URB2D - BLDFR_URB2D) * SW.
[[nodiscard]] double streetWidth(const UcpRow& row);
[[nodiscard]] double buildingWidth(const UcpRow& row);

// Ports _hgt_resampler: HGT_URB2D (area-weighted mean building height) -
// separately resamples a BW^2*MH_URB2D "nominator" and a BW^2
// "denominator" raster (both AVERAGE-resampled), then divides. NaN/0-by-0
// becomes 0.
[[nodiscard]] std::vector<float> hgtResample(const LczRaster& clean, const WrfGridInfo& grid, const std::vector<int>& builtLcz,
    const std::map<int, UcpRow>& ucpTable);

// Ports _lcz_resampler: majority-resamples the LCZ class raster (MODE
// resampling; non-built classes are excluded from the vote entirely, via
// NaN masking, NOT zero-masking like ucpResample - a zero would itself be
// counted as a losing "class 0" candidate) onto the WRF grid, renames LCZ
// 15 (paved) to 11 if built, and adds `addLczInt` (30 or 50, from
// WrfVersionInfo). Returns the full grid; the caller applies the
// FRC_URB2D-derived mask when writing into LU_INDEX, matching
// create_lcz_params_file's own `LU_INDEX.values[0, frc_mask] =
// lcz_resampled` - values at unmasked pixels (including any stray NaN the
// warp may have left) are never read.
[[nodiscard]] std::vector<float> lczResample(const LczRaster& clean, const WrfGridInfo& grid, const std::vector<int>& builtLcz, int addLczInt);

// Ports _compute_hi_distribution + _hi_resampler's aggregation:
// HI_URB2D's building-height-interval distribution (15 five-meter bins,
// 0-75m) per WRF grid pixel, plus NBUI_MAX (the largest number of nonzero
// bins at any one pixel - the value w2w's CLI reports as "Set nbui_max to
// N during compilation").
//
// Deliberately analytic, not Monte Carlo: w2w.py draws SAMPLE_SIZE=100000
// samples from scipy.stats.truncnorm and bins them - a real Monte Carlo
// estimate with no fixed seed anywhere in w2w's own code, so its own
// output isn't bit-reproducible run-to-run either. This computes the same
// truncated-normal distribution's bin probabilities directly via the
// standard normal CDF (std::erf) instead - the exact value w2w's own
// sampling is estimating, with zero sampling noise, not a different
// calculation. Pin against a live w2w run with a tolerance that accounts
// for ITS sampling noise, not this function's (which has none).
struct HiResampleResult {
    std::vector<float> hiUrb2d;  // 15 * ny * nx, percent (0-100) per height bin, row-major top-down per bin
    int nbuiMax{};
};
[[nodiscard]] HiResampleResult hiResample(const LczRaster& clean, const WrfGridInfo& grid, const std::vector<int>& builtLcz,
    const std::map<int, UcpRow>& ucpTable);

// Inputs to createLczParamsFile, grouped since it needs most of what the
// pipeline has produced so far.
struct LczParamsInputs {
    std::filesystem::path noUrbanPath;  // *_NoUrban.nc, already produced by removeUrban
    std::filesystem::path origPath;     // the ORIGINAL, unmodified geo_em file (info.dst_file)
    LczRaster clean;
    std::vector<int> builtLcz;
    std::map<int, UcpRow> ucpTable;
    WrfVersionInfo wrfVersion;
    double frcThreshold{};
};

// Ports create_lcz_params_file (w2w.py:1239-1348, add_wrf_version):
// resamples FRC_URB2D and the LCZ class raster onto the WRF grid, folds
// the six UCP fields plus HGT_URB2D and HI_URB2D into a 132-entry
// URB_PARAM block at their documented indices, grows LANDUSEF to
// `inputs.wrfVersion.numLandCat` categories (adjustGreenfracLandusef,
// below), and sets NUM_LAND_CAT/FLAG_URB_PARAM/NBUI_MAX. Writes the result
// to `outPath` (overwriting it if present) and returns NBUI_MAX.
int createLczParamsFile(const LczParamsInputs& inputs, const std::filesystem::path& outPath);

// Ports create_lcz_extent_file (w2w.py:1351-1397, add_wrf_version):
// derived from a file `createLczParamsFile` already produced - collapses
// its LCZ classes back to a single ISURBAN category, drops FRC_URB2D/
// URB_PARAM, and shrinks LANDUSEF back to `origPath`'s own original
// category count.
void createLczExtentFile(const std::filesystem::path& paramsPath, const std::filesystem::path& origPath, const std::filesystem::path& outPath);

// Ports expand_land_cat_parents (w2w.py, add_wrf_version): for each parent
// domain file (geo_em.d01.nc .. geo_em.d0{N-1}.nc, sitting beside `dstFile`
// - N is `dstFile`'s own two-digit domain number, read from its filename)
// whose NUM_LAND_CAT doesn't already match `wrfVersion.numLandCat`, grows
// its LANDUSEF to that many categories (the same zero-padding
// resizeDimension operation Stage 3 uses for LANDUSEF growth) and writes
// the result alongside the original with a `_{numLandCat}.nc` suffix -
// never modifies the parent file itself, and a domain already at the right
// count is left untouched (no file written). Returns one human-readable
// status line per parent domain number 1..N-1 (missing file / unreadable
// NUM_LAND_CAT-and-LANDUSEF / already correct / rewritten), for a UI to
// render as a checklist - this project has no print-to-stdout convention
// to port w2w.py's own colored print statements into.
[[nodiscard]] std::vector<std::string> expandLandCatParents(const std::filesystem::path& dstFile, const WrfVersionInfo& wrfVersion);

enum class CheckStatus { Ok, Warning };

struct CheckResult {
    std::string name;  // e.g. "Check 1: Urban class removed from geo_em.d04_NoUrban.nc?"
    CheckStatus status{};
    std::string message;
};

// The five file paths checksAndCleaning needs - a subset of w2w.py's own
// Info NamedTuple, by the point in the pipeline where checks run.
struct ChecksAndCleaningInputs {
    std::filesystem::path srcFileClean;      // *_clean.tif from checkLczIntegrity - removed as a final cleanup step
    std::filesystem::path dstFile;           // the ORIGINAL, unmodified geo_em file
    std::filesystem::path dstNuFile;         // *_NoUrban.nc
    std::filesystem::path dstLczExtentFile;  // *_LCZ_extent.nc
    std::filesystem::path dstLczParamsFile;  // *_LCZ_params.nc
};

// Ports checks_and_cleaning (w2w.py, add_wrf_version): nine sanity checks
// against the files the pipeline just produced, then deletes
// `inputs.srcFileClean` if present. `inputs.dstFile`'s own NUM_LAND_CAT
// selects which LU_INDEX values count as "urban" for checks 1, 3, and 9 (61
// -> [51..60, ISURBAN]; 41 -> [31..40, ISURBAN]; 20 or 21 -> [ISURBAN] only;
// anything else throws UserError) - matching add_wrf_version's four-way
// branch, since `main`'s bare-ISURBAN assumption is wrong once a
// 61-category source is possible. Check 8 is a real w2w.py quirk kept
// as-is: unlike checks 3/9, it does NOT use that branch's urban-class list -
// it always compares FRC_URB2D against a hardcoded `LU_INDEX >= 31`, even
// for a 61-category source (see PORT_W2W.MD Stage 4 for the read of the
// upstream source confirming this). `nbuiMax` (createLczParamsFile's
// return value) is NOT one of the nine checks - w2w's own CLI reports it
// separately as user-facing advice ("Set nbui_max to N during
// compilation"), which a UI surfaces directly rather than through this
// checklist.
[[nodiscard]] std::vector<CheckResult> checksAndCleaning(const ChecksAndCleaningInputs& inputs, const std::map<int, UcpRow>& ucpTable);

}  // namespace wrftools
