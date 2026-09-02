#pragma once

#include "wrftools/crs.hpp"
#include "wrftools/warp.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wrftools {

// Which wrfout files sharing a filename-parseable kind/domain (see
// groupWrfPaths) become one output NetCDF vs. one per input file.
enum class SeriesMode { Merge, PerFile };

// Caller-supplied replacements for computeDestinationGrid's own
// GDAL-suggested grid. Unset fields keep the suggestion; extent (in the
// TARGET CRS) must be given as all-four-or-none.
struct GridOverride {
    std::optional<double> pixelSizeX;
    std::optional<double> pixelSizeY;
    std::optional<Bounds2D> extent;
};

struct ReprojectOptions {
    // Either one plain file, or one complete groupWrfPaths() series - see
    // planReproject's doc comment for why mixing is rejected.
    std::vector<std::filesystem::path> inputs;
    int targetEpsg{4326};
    std::vector<std::string> variables;  // must be non-empty; names from WrfFile::variables()
    SeriesMode seriesMode{SeriesMode::Merge};
    std::filesystem::path outputDirectory;
    ResampleMethod resampling{ResampleMethod::Bilinear};
    bool nearestForCategorical{true};
    GridOverride grid;
    // An ncap2-like arithmetic-processor script (see derived_variable.hpp)
    // defining additional output variables computed from the selected
    // source variables - "" (the default) defines none. Every name the
    // script assigns is automatically included in the output, the same way
    // ncap2 itself writes every variable a script defines.
    std::string derivedVariablesScript;
};

// Everything the CF writer needs about the destination EPSG code, resolved
// once per run. Deliberately not routed through Crs: Crs stores a bare
// proj4 string, which drops the EPSG authority code that exportToCF1 needs
// to produce a proper named grid-mapping.
struct TargetCrsInfo {
    int epsg{};
    std::string wkt;                  // OAMS_TRADITIONAL_GIS_ORDER, for warpToGrid
    std::string crsWkt2;               // WKT2, for the crs_wkt/spatial_ref attributes
    bool isGeographic{};
    std::string xName;                 // "x" | "lon"
    std::string yName;                 // "y" | "lat"
    std::string xStandardName;         // "projection_x_coordinate" | "longitude"
    std::string yStandardName;         // "projection_y_coordinate" | "latitude"
    std::string xUnits;                // "m" | "degrees_east"
    std::string yUnits;                // "m" | "degrees_north"
    std::string gridMappingName;       // exportToCF1's grid_mapping_name
    std::vector<std::pair<std::string, std::string>> gridMappingAttributes;  // exportToCF1's own key/value list, verbatim
};

// Throws UserError for an EPSG code GDAL cannot resolve.
[[nodiscard]] TargetCrsInfo describeTargetCrs(int epsgCode);

// One entry of the EPSG CRS catalog, for a searchable projection picker -
// see listEpsgCrses.
struct EpsgCrsEntry {
    int code{};
    std::string name;
    std::string areaName;  // "" if the database has no area-of-use for this entry
    bool deprecated{};
};

// Every (non-deprecated by default) CRS in GDAL/PROJ's own "EPSG" authority
// database - projected and geographic alike, ~6000+ entries - sorted by
// code. Queried once and cached for the process's lifetime: the underlying
// OSRGetCRSInfoListFromDatabase call takes a noticeable fraction of a
// second, and this list is only ever needed to populate a UI picker, never
// on a hot path.
[[nodiscard]] const std::vector<EpsgCrsEntry>& listEpsgCrses();

// The destination grid computed ONCE per run: GDAL's own suggestion (see
// warp.hpp's suggestWarpGrid) with any GridOverride fields substituted in
// and width/height recomputed accordingly. Reused for every variable,
// level, and timestep of every output file in the run. Throws UserError on
// a degenerate result (non-finite geotransform, zero-sized, or absurdly
// large).
[[nodiscard]] DestinationGrid computeDestinationGrid(
    int sourceWidth, int sourceHeight, const std::string& sourceWkt, const std::array<double, 6>& sourceGeotransform, const TargetCrsInfo& target,
    const GridOverride& override);

struct ReprojectProgress {
    std::uint64_t completed{};
    std::uint64_t total{};
    std::string message;
};
using ReprojectProgressCallback = std::function<void(const ReprojectProgress&)>;

// Runs the whole conversion. Must be called on a thread that has not
// concurrently touched GDAL/netCDF-C elsewhere in the process - see
// reproject_worker.cpp, which runs this in its own single-threaded process
// specifically so the GUI never has to share that constraint (the
// documented HDF5 thread-affinity deadlock this project already hit for
// the LCZ pipeline - see lcz_form.hpp). Returns the paths written; throws
// UserError/UnsupportedError on any failure, and removes any output file it
// had already started writing.
[[nodiscard]] std::vector<std::filesystem::path> runReproject(const ReprojectOptions& options, const ReprojectProgressCallback& onProgress = {});

}  // namespace wrftools
