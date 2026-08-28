#include "wrftools/reproject.hpp"
#include "wrftools/derived_variable.hpp"
#include "wrftools/error.hpp"
#include "wrftools/netcdf_file.hpp"
#include "wrftools/wrf_file.hpp"
#include "wrftools/wrf_series.hpp"

#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <cpl_string.h>
#include <netcdf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>

namespace wrftools {
namespace {

constexpr float kWrfFillValue = 9.9692099683868690e36f;
constexpr double kGeotransformTolerance = 1e-6;

// Global attributes that describe the WRF projection/grid the reprojected
// file no longer has. Kept, not dropped - just renamed with a "WRF_" prefix
// - because leaving them under their canonical name is actively misleading:
// any WRF-aware reader (including this app's own WrfFile::buildWrfCrs)
// would happily reconstruct the OLD Mercator/Lambert/etc. grid from
// MAP_PROJ/TRUELAT1/... and place the reprojected raster in the wrong spot,
// even though the file's own CF grid_mapping/coordinates now say otherwise.
const std::set<std::string> kRenamedProjectionAttributes = {
    "MAP_PROJ", "MAP_PROJ_CHAR", "TRUELAT1", "TRUELAT2", "STAND_LON", "MOAD_CEN_LAT", "CEN_LAT", "CEN_LON", "POLE_LAT", "POLE_LON", "DX", "DY",
    "GRIDTYPE", "WEST-EAST_GRID_DIMENSION", "SOUTH-NORTH_GRID_DIMENSION", "BOTTOM-TOP_GRID_DIMENSION", "WEST-EAST_PATCH_START_UNSTAG",
    "WEST-EAST_PATCH_END_UNSTAG", "WEST-EAST_PATCH_START_STAG", "WEST-EAST_PATCH_END_STAG", "SOUTH-NORTH_PATCH_START_UNSTAG",
    "SOUTH-NORTH_PATCH_END_UNSTAG", "SOUTH-NORTH_PATCH_START_STAG", "SOUTH-NORTH_PATCH_END_STAG", "BOTTOM-TOP_PATCH_START_UNSTAG",
    "BOTTOM-TOP_PATCH_END_UNSTAG", "BOTTOM-TOP_PATCH_START_STAG", "BOTTOM-TOP_PATCH_END_STAG", "i_parent_start", "j_parent_start", "parent_grid_ratio",
    "corner_lats", "corner_lons"};

std::string isoTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", seconds);
}

std::string resampleMethodName(ResampleMethod method) {
    switch (method) {
        case ResampleMethod::Bilinear: return "bilinear";
        case ResampleMethod::Average: return "average";
        case ResampleMethod::Mode: return "mode";
        case ResampleMethod::Nearest: return "nearest";
    }
    return "nearest";
}

void report(const ReprojectProgressCallback& onProgress, std::uint64_t completed, std::uint64_t total, const std::string& message) {
    if (onProgress) onProgress({completed, total, message});
}

// A variable's effective OUTPUT vertical dimension name and level count,
// after vertical destaggering: a "*_stag" dimension (except
// soil_layers_stag, whose "stag" in the name is WRF naming convention, not
// a staggered grid - it holds soil-layer MIDPOINT depths) collapses to the
// unstaggered dimension of the same base name by averaging adjacent levels,
// which also makes its length coincide with that already-existing
// dimension - e.g. W's bottom_top_stag (65 levels) becomes 64 levels under
// "bottom_top", the same dimension T/P already use.
struct VerticalAxis {
    std::string dimensionName;  // empty for a 2D variable
    int rawLevelCount{1};       // source levels to read
    int outputLevelCount{1};    // levels after any destagger
    bool destagger{false};
};

VerticalAxis verticalAxisFor(const WrfVariable& variable) {
    if (!variable.extraDimension) return {};
    const auto& dim = *variable.extraDimension;
    const bool staggered = dim.size() > 5 && dim.rfind("_stag") == dim.size() - 5 && dim != "soil_layers_stag";
    if (staggered) return {dim.substr(0, dim.size() - 5), variable.levelCount, variable.levelCount - 1, true};
    return {dim, variable.levelCount, variable.levelCount, false};
}

std::vector<float> combineAdjacent(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> result(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) result[i] = (a[i] + b[i]) / 2.0f;
    return result;
}

void fillNanWithWrfFill(std::vector<float>& values) {
    for (auto& value : values)
        if (!std::isfinite(value)) value = kWrfFillValue;
}

// The OperandResolver a derived variable's evaluate() call needs, scoped to
// one (file, timestep): resolves a plain source-variable name by reading it
// RAW (WrfFile::read, no vertical destagger - see the comment on
// sourceShapes in runReproject), or an earlier-defined derived variable's
// own name by evaluating it recursively (self-referencing via std::ref, the
// same pattern the parser/evaluator's own tests use). Both kinds memoize
// per (name, level) so a name referenced more than once - within one
// expression, or by more than one derived variable at the same timestep -
// is only actually read/computed once; the cache is scoped to one timestep
// (a fresh DerivedResolver per t), matching readAllLevels' own per-call
// lifetime for pass-through variables.
struct DerivedResolver {
    WrfFile* file{};
    int localTime{};
    const std::vector<DerivedVariableDef>* defs{};
    std::map<std::string, std::map<int, std::vector<float>>> rawCache;
    std::map<std::string, std::map<int, std::vector<float>>> derivedCache;

    const std::vector<float>& operator()(const std::string& name, int level) {
        const auto& variables = file->variables();
        if (std::any_of(variables.begin(), variables.end(), [&](const WrfVariable& v) { return v.name == name; })) {
            auto& cacheForName = rawCache[name];
            if (const auto found = cacheForName.find(level); found != cacheForName.end()) return found->second;
            return cacheForName.emplace(level, file->read(name, localTime, level)).first->second;
        }
        auto& cacheForName = derivedCache[name];
        if (const auto found = cacheForName.find(level); found != cacheForName.end()) return found->second;
        const auto found = std::find_if(defs->begin(), defs->end(), [&](const auto& d) { return d.name == name; });
        if (found == defs->end())
            throw UserError("Internal error: derived variable script references unknown name '" + name + "'.");  // parseDerivedVariables already rejects this at parse time
        return cacheForName.emplace(level, evaluate(*found, level, std::ref(*this))).first->second;
    }
};

// Reads variable `name` at `timeIndex`, already vertically destaggered if
// applicable - returns one mass-grid plane per OUTPUT level.
std::vector<std::vector<float>> readAllLevels(WrfFile& file, const std::string& name, int timeIndex, const VerticalAxis& axis) {
    std::vector<std::vector<float>> raw(static_cast<std::size_t>(axis.rawLevelCount));
    for (int level = 0; level < axis.rawLevelCount; ++level) raw[static_cast<std::size_t>(level)] = file.read(name, timeIndex, level);
    if (!axis.destagger) return raw;
    std::vector<std::vector<float>> combined;
    combined.reserve(raw.size() - 1);
    for (std::size_t level = 0; level + 1 < raw.size(); ++level) combined.push_back(combineAdjacent(raw[level], raw[level + 1]));
    return combined;
}

// Per-record valid times for one input file, read from its own `Times`
// variable (authoritative - works for both a series' member files and a
// single multi-record file), falling back to the filename's own parsed
// time for a single-record file whose Times variable is missing or
// unparseable. Falls back further to std::nullopt (index-based time axis)
// only when neither source is usable.
struct FileTimes {
    std::vector<std::optional<std::chrono::sys_seconds>> values;
    std::vector<std::string> wrfStrings;  // verbatim "YYYY-MM-DD_HH:MM:SS", 19 chars, or padded
};

std::optional<std::chrono::sys_seconds> parseWrfTimeString(const std::string& text) {
    if (text.size() < 19) return std::nullopt;
    try {
        const int year = std::stoi(text.substr(0, 4));
        const unsigned month = static_cast<unsigned>(std::stoi(text.substr(5, 2)));
        const unsigned day = static_cast<unsigned>(std::stoi(text.substr(8, 2)));
        const int hour = std::stoi(text.substr(11, 2)), minute = std::stoi(text.substr(14, 2)), second = std::stoi(text.substr(17, 2));
        const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
        if (!date.ok() || hour > 23 || minute > 59 || second > 59) return std::nullopt;
        return std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

FileTimes readFileTimes(const std::filesystem::path& path) {
    FileTimes result;
    auto file = NetcdfFile::open(path, NetcdfFile::Mode::ReadOnly);
    if (file.hasVariable("Times")) {
        const auto shape = file.shape("Times");
        if (shape.size() == 2) {
            const auto text = file.readText("Times");
            const auto recordCount = shape[0], length = shape[1];
            for (std::size_t record = 0; record < recordCount; ++record) {
                auto row = text.substr(record * length, length);
                while (!row.empty() && row.back() == '\0') row.pop_back();
                result.wrfStrings.push_back(row);
                result.values.push_back(parseWrfTimeString(row));
            }
            return result;
        }
    }
    // No usable Times variable - fall back to the filename's own parsed
    // time, valid only for a single-record file.
    if (const auto parsed = parseWrfFilename(path)) {
        std::ostringstream label;
        const auto day = std::chrono::floor<std::chrono::days>(parsed->validTime);
        const std::chrono::year_month_day date{day};
        const auto clock = std::chrono::hh_mm_ss{parsed->validTime - day};
        label << int(date.year()) << '-' << std::setfill('0') << std::setw(2) << unsigned(date.month()) << '-' << std::setw(2) << unsigned(date.day())
              << '_' << std::setw(2) << clock.hours().count() << ':' << std::setw(2) << clock.minutes().count() << ':' << std::setw(2)
              << clock.seconds().count();
        result.wrfStrings.push_back(label.str());
        result.values.push_back(parsed->validTime);
        return result;
    }
    result.wrfStrings.push_back(std::string(19, ' '));
    result.values.push_back(std::nullopt);
    return result;
}

std::string sanitizedTimestampForFilename(const std::string& wrfTimeString) {
    std::string result = wrfTimeString;
    std::replace(result.begin(), result.end(), ':', '-');
    return result;
}

std::filesystem::path mergedOutputName(const std::vector<std::filesystem::path>& inputs, int epsg) {
    const auto first = parseWrfFilename(inputs.front()), last = parseWrfFilename(inputs.back());
    if (first && last) {
        return first->kind + "_d" + first->domain + "_" + sanitizedTimestampForFilename(formatWrfTimestamp(first->validTime)) + "_to_" +
               sanitizedTimestampForFilename(formatWrfTimestamp(last->validTime)) + "_epsg" + std::to_string(epsg) + ".nc";
    }
    return inputs.front().stem().string() + "_merged_epsg" + std::to_string(epsg) + ".nc";
}

// WrfFile/Crs build the source CRS on WRF's own modeling sphere
// (a=b=6370000, see crs.hpp) with NO datum linkage to WGS84 at all -
// deliberately, so the app's own internal geometry (map display, LCZ
// pipeline) never picks up a spurious datum shift. The values that sphere
// produces ARE, by WRF's own modeling convention, real-world WGS84 lat/lon
// (confirmed elsewhere in this codebase to agree with geogrid.exe's own
// XLAT_M/XLONG_M to within a few metres) - but PROJ has no way to know
// that from the CRS alone, since it carries no datum identity.
//
// Warping straight from that undefined-datum sphere to an EXTERNAL target
// CRS is fine when the target's own geographic base IS WGS84 (true of
// EPSG:4326 itself, every UTM zone, Web Mercator, ...): PROJ falls back to
// treating the numbers as an identity/no-shift, which is exactly correct
// there, since no real shift is needed. It silently gives the WRONG answer
// - by anywhere from tens to hundreds of metres, not a rounding error -
// when the target uses a genuinely different (usually legacy/local) datum,
// e.g. EPSG:2326 (Hong Kong 1980 Grid): PROJ has a real, published
// WGS84->HK1980 transformation, but never finds it, because the source
// CRS's datum was never asserted to BE WGS84 in the first place - it just
// silently no-ops instead of erroring. Confirmed empirically: comparing a
// direct sphere->EPSG:2326 point transform against WGS84->EPSG:2326 for
// Hong Kong's own domain centre differs by ~300 m in easting/northing.
//
// Fixed with an explicit two-stage warp whenever the target's own
// geographic base differs from real WGS84 (IsSameGeogCS false): first warp
// from the source sphere to an intermediate grid on that SAME sphere in
// geographic (lon/lat) space - an exact, distortion-free unprojection,
// since source and intermediate share one identical GEOGCS - then RELABEL
// that intermediate's CRS as literal EPSG:4326 (legitimate given the
// accuracy note above: the numbers already ARE meant to be WGS84 degrees)
// and warp AGAIN from that properly-labelled WGS84 raster to the real
// target CRS, where PROJ now finds and applies the genuine datum
// transformation. This costs one extra resampling pass, only when the
// target's datum actually differs from WGS84 - the common case (UTM,
// EPSG:4326, Web Mercator, ...) stays a single warp, unchanged.
bool targetSharesWgs84Datum(const std::string& targetWkt) {
    OGRSpatialReference target;
    target.importFromWkt(targetWkt.c_str());
    OGRSpatialReference wgs84;
    wgs84.importFromEPSG(4326);
    return target.IsSameGeogCS(&wgs84) != 0;
}

std::string wgs84PivotWkt() {
    OGRSpatialReference wgs84;
    wgs84.importFromEPSG(4326);
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    // The zero-argument std::string-returning exportToWkt() overload is a
    // newer convenience API than this project's own GDAL floor (see
    // describeTargetCrs's exportToCF1 comment) - the char**-returning form
    // used here has existed since GDAL 2.x, so it works on every GDAL this
    // project targets, not just the ones new enough for the convenience one.
    char* wktRaw = nullptr;
    wgs84.exportToWkt(&wktRaw);
    const std::string wkt = wktRaw ? wktRaw : "";
    CPLFree(wktRaw);
    return wkt;
}

// Ties suggestWarpGrid's own georeferencing-only query into the two-stage
// datum-safe scheme above, so the SUGGESTED grid's position (used both to
// size/place the final output grid and, via the SAME intermediate, to warp
// every slice's actual data - see runReproject) is consistent throughout:
// getting these out of sync would place the coordinate axes correctly but
// warp the data onto the old (wrong) position, or vice versa.
struct DatumSafeSuggestion {
    bool targetSharesWgs84{};
    DestinationGrid suggested;             // in target CRS, correctly positioned
    DestinationGrid intermediateWgs84Grid;  // meaningful only if !targetSharesWgs84
};

DatumSafeSuggestion suggestWarpGridDatumSafe(
    int sourceWidth, int sourceHeight, const std::string& sourceWkt, const std::array<double, 6>& sourceGeotransform, const std::string& targetWkt) {
    if (targetSharesWgs84Datum(targetWkt))
        return {true, suggestWarpGrid(sourceWidth, sourceHeight, sourceWkt, sourceGeotransform, targetWkt), {}};
    const auto intermediate = suggestWarpGrid(sourceWidth, sourceHeight, sourceWkt, sourceGeotransform, Crs::lonLat().wkt());
    // suggestWarpGrid only ever needs georeferencing, not real pixel data
    // (see warp.cpp), so relabeling the intermediate's CRS as literal WGS84
    // and asking again for the target's own suggested grid is exactly the
    // same query the eventual data warp will make - no raster is touched
    // here, this is purely a geotransform/extent computation.
    const auto suggested = suggestWarpGrid(intermediate.width, intermediate.height, wgs84PivotWkt(), intermediate.geotransform, targetWkt);
    return {false, suggested, intermediate};
}

void checkSameGrid(const WrfFile& first, const WrfFile& other, const std::filesystem::path& otherPath) {
    if (other.projectionWkt() != first.projectionWkt() || other.size() != first.size())
        throw UserError("Input files have incompatible grids: " + otherPath.string());
    const auto &a = first.geotransform(), &b = other.geotransform();
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > kGeotransformTolerance) throw UserError("Input files have incompatible grids: " + otherPath.string());
}

}  // namespace

TargetCrsInfo describeTargetCrs(int epsgCode) {
    GDALAllRegister();  // exportToCF1 below silently degrades (grid_mapping_name "crs", no parameters) if GDAL's drivers/PROJ integration were never initialized
    OGRSpatialReference srs;
    if (srs.importFromEPSG(epsgCode) != OGRERR_NONE) throw UserError("Unknown or unsupported EPSG code: " + std::to_string(epsgCode));
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    TargetCrsInfo info;
    info.epsg = epsgCode;
    char* wktRaw = nullptr;
    if (srs.exportToWkt(&wktRaw) != OGRERR_NONE || !wktRaw) throw UserError("Could not build the target CRS for EPSG:" + std::to_string(epsgCode));
    info.wkt = wktRaw;
    CPLFree(wktRaw);
    // WKT2 (the default "options=nullptr" behaviour of the newer
    // std::string-returning exportToWkt() overload) via the same portable
    // char**-based call, requesting the FORMAT=WKT2 option explicitly -
    // exportToWkt(char**) defaults to WKT1 without it.
    char* wkt2Raw = nullptr;
    const char* wkt2Options[] = {"FORMAT=WKT2", nullptr};
    srs.exportToWkt(&wkt2Raw, wkt2Options);
    info.crsWkt2 = wkt2Raw ? wkt2Raw : "";
    CPLFree(wkt2Raw);
    info.isGeographic = srs.IsGeographic() != 0;

    // exportToCF1 requires GDAL >= 3.9 (see CMakeLists.txt's
    // find_package(GDAL ...) minimum) - its first out-param is GDAL's
    // RECOMMENDED VARIABLE NAME for the grid mapping (e.g. "crs"), not the
    // grid_mapping_name attribute value itself, which instead comes back as
    // one entry inside the key/value list (confirmed empirically: for
    // EPSG:4326 it returns name="crs" and keyValues containing
    // "grid_mapping_name=latitude_longitude" separately). "crs_wkt" is
    // excluded from gridMappingAttributes below because this function
    // already builds its own (WKT2, via crsWkt2) - keeping GDAL's WKT1 one
    // too would just be a silently-overwritten duplicate attribute name at
    // write time.
    char* recommendedVariableName = nullptr;
    char** keyValues = nullptr;
    char* units = nullptr;
    const auto cfStatus = srs.exportToCF1(&recommendedVariableName, &keyValues, &units, nullptr);
    if (cfStatus == OGRERR_NONE) {
        for (int i = 0; keyValues && keyValues[i]; ++i) {
            const std::string entry = keyValues[i];
            const auto eq = entry.find('=');
            if (eq == std::string::npos) continue;
            const auto key = entry.substr(0, eq), value = entry.substr(eq + 1);
            if (key == "grid_mapping_name") info.gridMappingName = value;
            else if (key != "crs_wkt") info.gridMappingAttributes.emplace_back(key, value);
        }
    }
    CPLFree(recommendedVariableName);
    CSLDestroy(keyValues);

    if (info.isGeographic) {
        info.xName = "lon";
        info.yName = "lat";
        info.xStandardName = "longitude";
        info.yStandardName = "latitude";
        info.xUnits = "degrees_east";
        info.yUnits = "degrees_north";
        if (info.gridMappingName.empty()) info.gridMappingName = "latitude_longitude";
    } else {
        info.xName = "x";
        info.yName = "y";
        info.xStandardName = "projection_x_coordinate";
        info.yStandardName = "projection_y_coordinate";
        info.xUnits = units ? units : "m";
        info.yUnits = info.xUnits;
    }
    CPLFree(units);
    return info;
}

DestinationGrid computeDestinationGrid(int sourceWidth, int sourceHeight, const std::string& sourceWkt, const std::array<double, 6>& sourceGeotransform,
    const TargetCrsInfo& target, const GridOverride& override) {
    auto grid = suggestWarpGridDatumSafe(sourceWidth, sourceHeight, sourceWkt, sourceGeotransform, target.wkt).suggested;

    double minX = grid.geotransform[0], maxY = grid.geotransform[3];
    double maxX = minX + grid.geotransform[1] * grid.width, minY = maxY + grid.geotransform[5] * grid.height;
    if (override.extent) {
        minX = override.extent->minX;
        minY = override.extent->minY;
        maxX = override.extent->maxX;
        maxY = override.extent->maxY;
    }
    const double pixelSizeX = override.pixelSizeX.value_or(grid.geotransform[1]);
    const double pixelSizeY = override.pixelSizeY.value_or(-grid.geotransform[5]);
    if (!(pixelSizeX > 0.0) || !(pixelSizeY > 0.0)) throw UserError("The output pixel size must be a positive number.");

    // Computed in double throughout, and range-checked BEFORE narrowing to
    // int - a degenerate pixel size (e.g. a user-typo'd 1e-12) can demand
    // 10^14+ pixels on an axis, and static_cast<int> of a double that far
    // outside int's range is undefined behaviour, not a large-but-valid
    // int - it must never be computed, let alone compared against
    // kMaxPixels, before this check runs.
    const double widthDouble = std::ceil((maxX - minX) / pixelSizeX);
    const double heightDouble = std::ceil((maxY - minY) / pixelSizeY);
    constexpr double kMaxPixels = 200'000'000.0;
    if (!std::isfinite(minX) || !std::isfinite(maxY) || !std::isfinite(widthDouble) || !std::isfinite(heightDouble) || widthDouble < 1.0 ||
        heightDouble < 1.0)
        throw UserError("Could not compute a valid output grid for the requested CRS/extent.");
    if (widthDouble * heightDouble > kMaxPixels)
        throw UserError("The requested output grid is " + std::to_string(static_cast<long long>(widthDouble)) + " x " +
                         std::to_string(static_cast<long long>(heightDouble)) + " pixels - increase the pixel size or reduce the extent.");
    const int width = static_cast<int>(widthDouble);
    const int height = static_cast<int>(heightDouble);

    DestinationGrid result;
    result.wkt = target.wkt;
    result.geotransform = {minX, pixelSizeX, 0.0, maxY, 0.0, -pixelSizeY};
    result.width = width;
    result.height = height;
    return result;
}

std::vector<std::filesystem::path> runReproject(const ReprojectOptions& options, const ReprojectProgressCallback& onProgress) {
    if (options.inputs.empty()) throw UserError("No input files were selected.");
    // Not "options.variables.empty()" alone - a run defining only derived
    // variables (options.variables empty, derivedVariablesScript non-empty)
    // is valid; the real emptiness check is deferred until derivedVariables
    // is parsed below, once both sources of output variables are known.
    if (options.variables.empty() && options.derivedVariablesScript.find_first_not_of(" \t\r\n") == std::string::npos)
        throw UserError("No variables were selected.");
    if (options.outputDirectory.empty()) throw UserError("No output directory was selected.");
    std::error_code dirError;
    if (!std::filesystem::is_directory(options.outputDirectory, dirError))
        throw UserError("Output directory does not exist: " + options.outputDirectory.string());

    const auto grouped = groupWrfPaths(options.inputs);
    if (grouped.groups.size() + grouped.singles.size() != 1)
        throw UserError("Select exactly one wrfout file, or one complete wrfout series, to reproject at a time.");
    const bool isSeries = !grouped.groups.empty();
    const std::vector<std::filesystem::path> orderedInputs = isSeries ? grouped.groups.front() : grouped.singles;

    // Open every input up front - validates every selected variable and
    // every grid BEFORE any output file is created, rather than failing
    // mid-write.
    std::map<std::string, std::unique_ptr<WrfFile>> opened;
    for (const auto& path : orderedInputs) opened.emplace(path.string(), std::make_unique<WrfFile>(path));
    auto& firstFile = *opened.at(orderedInputs.front().string());
    for (const auto& path : orderedInputs) checkSameGrid(firstFile, *opened.at(path.string()), path);

    std::vector<WrfVariable> selectedVariables;
    for (const auto& name : options.variables) {
        const WrfVariable* info = nullptr;
        for (const auto& path : orderedInputs) {
            const auto& variables = opened.at(path.string())->variables();
            const auto found = std::find_if(variables.begin(), variables.end(), [&](const auto& v) { return v.name == name; });
            if (found == variables.end()) throw UserError("Variable '" + name + "' is not available in " + path.filename().string() + ".");
            if (!info) info = &*found;
        }
        selectedVariables.push_back(*info);
    }

    // Every variable available across the opened input(s), by its RAW
    // (undestaggered) dimension - exactly WrfVariable's own
    // extraDimension/levelCount, no verticalAxisFor collapsing. A derived
    // expression must see PH/PHB at their full staggered level count (see
    // derived_variable.hpp's own note on why - the file this project was
    // asked to reproduce, an ncap2 LVLHT/PTOT/TK script, only computes the
    // right answer from PH/PHB's raw stack, not the destaggered one this
    // file's own pass-through path would otherwise silently substitute).
    std::map<std::string, VariableShape> sourceShapes;
    for (const auto& variable : firstFile.variables())
        sourceShapes.emplace(variable.name, VariableShape{variable.extraDimension.value_or(""), variable.levelCount});
    const auto derivedVariables = parseDerivedVariables(options.derivedVariablesScript, sourceShapes);

    // A derived variable that overrides a source variable's own name (e.g.
    // `T2 = T2 - 273.15;`) supersedes any pass-through copy of that name -
    // drop it from selectedVariables here so it's defined/written exactly
    // once (by the derived-variable path below), never twice, whether or
    // not the user also happened to tick it in the variable list.
    selectedVariables.erase(std::remove_if(selectedVariables.begin(), selectedVariables.end(),
                                 [&](const WrfVariable& variable) {
                                     return std::any_of(derivedVariables.begin(), derivedVariables.end(),
                                         [&](const DerivedVariableDef& def) { return def.overridesSourceVariable && def.name == variable.name; });
                                 }),
        selectedVariables.end());

    const TargetCrsInfo target = describeTargetCrs(options.targetEpsg);
    const DestinationGrid grid =
        computeDestinationGrid(firstFile.size()[0], firstFile.size()[1], firstFile.projectionWkt(), firstFile.geotransform(), target, options.grid);

    // Reuses the SAME datum-safe suggestion computeDestinationGrid already
    // made internally (a cheap, data-free georeferencing query - see
    // DatumSafeSuggestion's own comment) so the intermediate grid used to
    // warp every slice's actual data below is consistent with the
    // coordinate axes `grid` above already committed to.
    const auto datumSafety =
        suggestWarpGridDatumSafe(firstFile.size()[0], firstFile.size()[1], firstFile.projectionWkt(), firstFile.geotransform(), target.wkt);
    const bool targetIsWgs84Compatible = datumSafety.targetSharesWgs84;
    const std::string wgs84Wkt = targetIsWgs84Compatible ? std::string{} : wgs84PivotWkt();
    const DestinationGrid& intermediateWgs84Grid = datumSafety.intermediateWgs84Grid;

    // The single warp call every slice goes through - see
    // targetSharesWgs84Datum's comment for why a datum-differing target
    // needs the extra hop via a properly WGS84-labelled intermediate grid,
    // rather than warping straight from the source's undefined-datum
    // sphere to the target.
    auto warpSlice = [&](const std::vector<float>& source, int sourceWidth, int sourceHeight, const std::string& sourceWkt,
                          const std::array<double, 6>& sourceGeotransform, ResampleMethod resampling) -> std::vector<float> {
        if (targetIsWgs84Compatible)
            return warpToGrid(source, sourceWidth, sourceHeight, sourceWkt, sourceGeotransform, grid.wkt, grid.geotransform, grid.width, grid.height,
                resampling);
        const auto intermediate = warpToGrid(source, sourceWidth, sourceHeight, sourceWkt, sourceGeotransform, Crs::lonLat().wkt(),
            intermediateWgs84Grid.geotransform, intermediateWgs84Grid.width, intermediateWgs84Grid.height, resampling);
        return warpToGrid(intermediate, intermediateWgs84Grid.width, intermediateWgs84Grid.height, wgs84Wkt, intermediateWgs84Grid.geotransform, grid.wkt,
            grid.geotransform, grid.width, grid.height, resampling);
    };

    struct OutputJob {
        std::filesystem::path path;
        std::vector<std::filesystem::path> inputs;
    };
    std::vector<OutputJob> jobs;
    const std::string suffix = "_epsg" + std::to_string(options.targetEpsg) + ".nc";
    if (!isSeries || options.seriesMode == SeriesMode::PerFile) {
        for (const auto& input : orderedInputs) jobs.push_back({options.outputDirectory / (input.stem().string() + suffix), {input}});
    } else {
        jobs.push_back({options.outputDirectory / mergedOutputName(orderedInputs, options.targetEpsg), orderedInputs});
    }

    // Vertical axes needed by the selected AND derived variables,
    // deduplicated by name. A derived variable's axis uses its own RAW
    // shape (shapeOf) directly - never verticalAxisFor's destagger - so it
    // naturally lands on a different dimension name than a pass-through
    // *_stag variable would (e.g. "bottom_top_stag"/65 for a derived LVLHT
    // vs. the destaggered "bottom_top"/64 a pass-through-selected PH would
    // use), with no name collision between the two paths; an UNstaggered
    // dimension (e.g. "bottom_top" itself) is written identically by both
    // paths and so is naturally shared, which is exactly what's wanted when
    // e.g. a pass-through T and a derived PTOT both belong on it.
    std::vector<std::pair<std::string, VerticalAxis>> axes;  // name -> axis info (rawLevelCount/outputLevelCount as seen for THIS name)
    auto addAxis = [&](const std::string& dimensionName, int outputLevelCount, const VerticalAxis& axis) {
        if (dimensionName.empty()) return;
        const auto found = std::find_if(axes.begin(), axes.end(), [&](const auto& entry) { return entry.first == dimensionName; });
        if (found == axes.end()) axes.emplace_back(dimensionName, axis);
        else if (found->second.outputLevelCount != outputLevelCount)
            throw UserError("Vertical dimension '" + dimensionName + "' has inconsistent lengths across selected/derived variables.");
    };
    for (const auto& variable : selectedVariables) {
        const auto axis = verticalAxisFor(variable);
        addAxis(axis.dimensionName, axis.outputLevelCount, axis);
    }
    for (const auto& def : derivedVariables) {
        const auto shape = shapeOf(def);
        addAxis(shape.dimensionName, shape.levelCount, VerticalAxis{shape.dimensionName, shape.levelCount, shape.levelCount, false});
    }

    std::uint64_t totalSlices = 0;
    for (const auto& job : jobs) {
        std::uint64_t timeCount = 0;
        for (const auto& input : job.inputs) timeCount += readFileTimes(input).values.size();
        for (const auto& variable : selectedVariables) totalSlices += timeCount * static_cast<std::uint64_t>(verticalAxisFor(variable).outputLevelCount);
        for (const auto& def : derivedVariables) totalSlices += timeCount * static_cast<std::uint64_t>(shapeOf(def).levelCount);
    }
    std::uint64_t completed = 0;
    std::vector<std::filesystem::path> written;

    for (const auto& job : jobs) {
        report(onProgress, completed, totalSlices, "Writing " + job.path.filename().string());

        std::vector<std::string> wrfTimeStrings;
        std::vector<std::optional<std::chrono::sys_seconds>> timeValues;
        std::vector<std::pair<std::filesystem::path, int>> timeMap;  // (file, local record index) per output time index
        for (const auto& input : job.inputs) {
            const auto times = readFileTimes(input);
            for (std::size_t record = 0; record < times.values.size(); ++record) {
                wrfTimeStrings.push_back(times.wrfStrings.at(record));
                timeValues.push_back(times.values.at(record));
                timeMap.emplace_back(input, static_cast<int>(record));
            }
        }
        const std::size_t timeCount = wrfTimeStrings.size();

        const auto firstValidTime = std::find_if(timeValues.begin(), timeValues.end(), [](const auto& v) { return v.has_value(); });
        const bool haveRealTimes = firstValidTime != timeValues.end();
        const auto epoch = haveRealTimes ? **firstValidTime : std::chrono::sys_seconds{};

        try {
            auto out = NetcdfFile::create(job.path, NetcdfFile::Format::Netcdf4Classic);

            out.defineDimension("time", timeCount);
            out.defineDimension("DateStrLen", 19);
            out.defineDimension(target.yName, static_cast<std::size_t>(grid.height));
            out.defineDimension(target.xName, static_cast<std::size_t>(grid.width));
            for (const auto& [name, axis] : axes) out.defineDimension(name, static_cast<std::size_t>(axis.outputLevelCount));

            out.defineVariable(target.xName, NC_DOUBLE, {target.xName});
            out.defineVariable(target.yName, NC_DOUBLE, {target.yName});
            out.defineVariable("time", NC_DOUBLE, {"time"});
            out.defineVariable("Times", NC_CHAR, {"time", "DateStrLen"});
            out.defineVariable("crs", NC_INT, {});
            for (const auto& [name, axis] : axes) out.defineVariable(name, NC_FLOAT, {name});

            for (const auto& variable : selectedVariables) {
                const auto axis = verticalAxisFor(variable);
                std::vector<std::string> dims{"time"};
                if (!axis.dimensionName.empty()) dims.push_back(axis.dimensionName);
                dims.push_back(target.yName);
                dims.push_back(target.xName);
                std::vector<std::size_t> chunk{1};
                if (!axis.dimensionName.empty()) chunk.push_back(1);
                chunk.push_back(static_cast<std::size_t>(grid.height));
                chunk.push_back(static_cast<std::size_t>(grid.width));
                out.defineVariable(variable.name, NC_FLOAT, dims, chunk, /*deflateLevel=*/4, kWrfFillValue);
            }
            for (const auto& def : derivedVariables) {
                const auto shape = shapeOf(def);
                std::vector<std::string> dims{"time"};
                if (!shape.dimensionName.empty()) dims.push_back(shape.dimensionName);
                dims.push_back(target.yName);
                dims.push_back(target.xName);
                std::vector<std::size_t> chunk{1};
                if (!shape.dimensionName.empty()) chunk.push_back(1);
                chunk.push_back(static_cast<std::size_t>(grid.height));
                chunk.push_back(static_cast<std::size_t>(grid.width));
                out.defineVariable(def.name, NC_FLOAT, dims, chunk, /*deflateLevel=*/4, kWrfFillValue);
            }

            // ---- attributes ----
            out.putAttribute(target.xName, {"standard_name", NC_CHAR, target.xStandardName, {}});
            out.putAttribute(target.xName, {"long_name", NC_CHAR, target.isGeographic ? "longitude" : "x coordinate of projection", {}});
            out.putAttribute(target.xName, {"units", NC_CHAR, target.xUnits, {}});
            out.putAttribute(target.xName, {"axis", NC_CHAR, "X", {}});
            out.putAttribute(target.yName, {"standard_name", NC_CHAR, target.yStandardName, {}});
            out.putAttribute(target.yName, {"long_name", NC_CHAR, target.isGeographic ? "latitude" : "y coordinate of projection", {}});
            out.putAttribute(target.yName, {"units", NC_CHAR, target.yUnits, {}});
            out.putAttribute(target.yName, {"axis", NC_CHAR, "Y", {}});

            const std::string timeUnits =
                haveRealTimes ? "seconds since " + std::format("{:%Y-%m-%d %H:%M:%S}", epoch) : "1";
            out.putAttribute("time", {"standard_name", NC_CHAR, "time", {}});
            out.putAttribute("time", {"long_name", NC_CHAR, haveRealTimes ? "time" : "time step index", {}});
            out.putAttribute("time", {"units", NC_CHAR, timeUnits, {}});
            if (haveRealTimes) out.putAttribute("time", {"calendar", NC_CHAR, "standard", {}});
            out.putAttribute("time", {"axis", NC_CHAR, "T", {}});

            out.putAttribute("crs", {"grid_mapping_name", NC_CHAR, target.gridMappingName, {}});
            for (const auto& [key, value] : target.gridMappingAttributes) {
                try {
                    out.putAttribute("crs", {key, NC_DOUBLE, "", {std::stod(value)}});
                } catch (const std::exception&) {
                    out.putAttribute("crs", {key, NC_CHAR, value, {}});
                }
            }
            out.putAttribute("crs", {"crs_wkt", NC_CHAR, target.crsWkt2, {}});
            out.putAttribute("crs", {"spatial_ref", NC_CHAR, target.crsWkt2, {}});
            out.putAttribute("crs", {"epsg_code", NC_CHAR, "EPSG:" + std::to_string(target.epsg), {}});
            {
                std::ostringstream gt;
                gt << std::setprecision(17) << grid.geotransform[0] << ' ' << grid.geotransform[1] << ' ' << grid.geotransform[2] << ' '
                   << grid.geotransform[3] << ' ' << grid.geotransform[4] << ' ' << grid.geotransform[5];
                out.putAttribute("crs", {"GeoTransform", NC_CHAR, gt.str(), {}});
            }

            for (const auto& [name, axis] : axes) {
                out.putAttribute(name, {"axis", NC_CHAR, "Z", {}});
                out.putAttribute(name, {"long_name", NC_CHAR, "model " + name + " level", {}});
                out.putAttribute(name, {"units", NC_CHAR, "1", {}});
                if (name == "soil_layers_stag") out.putAttribute(name, {"positive", NC_CHAR, "down", {}});
            }

            for (const auto& variable : selectedVariables) {
                if (!variable.description.empty()) out.putAttribute(variable.name, {"long_name", NC_CHAR, variable.description, {}});
                if (!variable.units.empty()) out.putAttribute(variable.name, {"units", NC_CHAR, variable.units, {}});
                out.putAttribute(variable.name, {"grid_mapping", NC_CHAR, "crs", {}});
                out.putAttribute(variable.name, {"missing_value", NC_FLOAT, "", {static_cast<double>(kWrfFillValue)}});
                if (variable.categoryScheme) out.putAttribute(variable.name, {"wrf_category_scheme", NC_CHAR, *variable.categoryScheme, {}});
            }
            for (const auto& def : derivedVariables) {
                // An override (def.name reassigns a source variable, e.g.
                // `T2 = T2 - 273.15;`) falls back to the ORIGINAL variable's
                // own long_name/units when the script doesn't set them
                // itself - closest to "still logically named T2, so still
                // looks like T2 unless told otherwise". This is exactly the
                // gotcha ncap2 itself has too when a transformed variable's
                // units genuinely change (K -> degC here) but the script
                // never overrides @units: the attribute would then be
                // stale/wrong, not a bug in this fallback - the script has
                // to say so explicitly.
                std::string longName = def.longName, units = def.units;
                if (def.overridesSourceVariable && (longName.empty() || units.empty())) {
                    const auto found = std::find_if(
                        firstFile.variables().begin(), firstFile.variables().end(), [&](const WrfVariable& v) { return v.name == def.name; });
                    if (found != firstFile.variables().end()) {
                        if (longName.empty()) longName = found->description;
                        if (units.empty()) units = found->units;
                    }
                }
                if (!longName.empty()) out.putAttribute(def.name, {"long_name", NC_CHAR, longName, {}});
                if (!units.empty()) out.putAttribute(def.name, {"units", NC_CHAR, units, {}});
                out.putAttribute(def.name, {"grid_mapping", NC_CHAR, "crs", {}});
                out.putAttribute(def.name, {"missing_value", NC_FLOAT, "", {static_cast<double>(kWrfFillValue)}});
            }

            // Global attributes: copy the source's, except the
            // projection/grid ones (renamed WRF_<name> - see
            // kRenamedProjectionAttributes' own comment).
            {
                auto source = NetcdfFile::open(job.inputs.front(), NetcdfFile::Mode::ReadOnly);
                for (const auto& attribute : source.attributes("")) {
                    if (kRenamedProjectionAttributes.contains(attribute.name)) {
                        auto renamed = attribute;
                        renamed.name = "WRF_" + attribute.name;
                        out.putAttribute("", renamed);
                    } else {
                        out.putAttribute("", attribute);
                    }
                }
            }
            out.putAttribute("", {"Conventions", NC_CHAR, "CF-1.7", {}});
            std::ostringstream history;
            history << isoTimestamp() << ": wrftools reproject to EPSG:" << target.epsg << " (resampling=" << resampleMethodName(options.resampling)
                    << ", grid=" << grid.width << "x" << grid.height << ")";
            out.putAttribute("", {"history", NC_CHAR, history.str(), {}});
            {
                std::ostringstream sources;
                for (std::size_t i = 0; i < job.inputs.size(); ++i) { if (i) sources << ";"; sources << job.inputs[i].filename().string(); }
                out.putAttribute("", {"wrftools_source_files", NC_CHAR, sources.str(), {}});
            }

            // ---- coordinate values ----
            {
                std::vector<double> xValues(static_cast<std::size_t>(grid.width)), yValues(static_cast<std::size_t>(grid.height));
                for (int i = 0; i < grid.width; ++i) xValues[static_cast<std::size_t>(i)] = grid.geotransform[0] + (i + 0.5) * grid.geotransform[1];
                for (int j = 0; j < grid.height; ++j) yValues[static_cast<std::size_t>(j)] = grid.geotransform[3] + (j + 0.5) * grid.geotransform[5];
                out.writeDouble(target.xName, xValues);
                out.writeDouble(target.yName, yValues);
            }
            {
                std::vector<double> timeSeconds(timeCount);
                for (std::size_t i = 0; i < timeCount; ++i)
                    timeSeconds[i] = haveRealTimes && timeValues[i] ? static_cast<double>((*timeValues[i] - epoch).count()) : static_cast<double>(i);
                out.writeDouble("time", timeSeconds);
                std::string timesText;
                for (const auto& text : wrfTimeStrings) { auto padded = text; padded.resize(19, ' '); timesText += padded; }
                out.writeText("Times", timesText);
            }
            for (const auto& [name, axis] : axes) {
                std::vector<float> values(static_cast<std::size_t>(axis.outputLevelCount));
                std::iota(values.begin(), values.end(), 0.0f);
                out.writeFloat(name, values);
            }

            // ---- data, one (time, [level]) plane at a time ----
            for (const auto& variable : selectedVariables) {
                const auto axis = verticalAxisFor(variable);
                const auto resampling = (variable.categoryScheme && options.nearestForCategorical) ? ResampleMethod::Nearest : options.resampling;
                bool sawFiniteData = false;
                for (std::size_t t = 0; t < timeCount; ++t) {
                    const auto& [path, localTime] = timeMap[t];
                    auto& file = *opened.at(path.string());
                    const auto levels = readAllLevels(file, variable.name, localTime, axis);
                    for (int level = 0; level < axis.outputLevelCount; ++level) {
                        const auto& source = levels[static_cast<std::size_t>(level)];
                        const auto& size = file.size();
                        auto warped = warpSlice(source, size[0], size[1], file.projectionWkt(), file.geotransform(), resampling);
                        for (float value : warped)
                            if (std::isfinite(value)) { sawFiniteData = true; break; }
                        fillNanWithWrfFill(warped);

                        std::vector<std::size_t> start{t};
                        std::vector<std::size_t> count{1};
                        if (!axis.dimensionName.empty()) { start.push_back(static_cast<std::size_t>(level)); count.push_back(1); }
                        start.push_back(0);
                        start.push_back(0);
                        count.push_back(static_cast<std::size_t>(grid.height));
                        count.push_back(static_cast<std::size_t>(grid.width));
                        out.writeFloatSlice(variable.name, start, count, warped);

                        ++completed;
                        report(onProgress, completed, totalSlices, job.path.filename().string() + ": " + variable.name);
                    }
                }
                if (!sawFiniteData)
                    report(onProgress, completed, totalSlices, "Warning: " + variable.name + " in " + job.path.filename().string() + " is entirely fill/nodata.");
            }
            for (const auto& def : derivedVariables) {
                const auto shape = shapeOf(def);
                bool sawFiniteData = false;
                for (std::size_t t = 0; t < timeCount; ++t) {
                    const auto& [path, localTime] = timeMap[t];
                    auto& file = *opened.at(path.string());
                    DerivedResolver resolver{&file, localTime, &derivedVariables, {}, {}};
                    for (int level = 0; level < shape.levelCount; ++level) {
                        auto values = evaluate(def, level, std::ref(resolver));
                        const auto& size = file.size();
                        auto warped = warpSlice(values, size[0], size[1], file.projectionWkt(), file.geotransform(), options.resampling);
                        for (float value : warped)
                            if (std::isfinite(value)) { sawFiniteData = true; break; }
                        fillNanWithWrfFill(warped);

                        std::vector<std::size_t> start{t};
                        std::vector<std::size_t> count{1};
                        if (!shape.dimensionName.empty()) { start.push_back(static_cast<std::size_t>(level)); count.push_back(1); }
                        start.push_back(0);
                        start.push_back(0);
                        count.push_back(static_cast<std::size_t>(grid.height));
                        count.push_back(static_cast<std::size_t>(grid.width));
                        out.writeFloatSlice(def.name, start, count, warped);

                        ++completed;
                        report(onProgress, completed, totalSlices, job.path.filename().string() + ": " + def.name);
                    }
                }
                if (!sawFiniteData)
                    report(onProgress, completed, totalSlices, "Warning: " + def.name + " in " + job.path.filename().string() + " is entirely fill/nodata.");
            }

            out.close();
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(job.path, ignored);
            throw;
        }
        written.push_back(job.path);
    }
    return written;
}

}  // namespace wrftools
