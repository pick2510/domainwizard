#include "wrftools/wrf_file.hpp"
#include "wrftools/crs.hpp"
#include "wrftools/error.hpp"

#include <gdal_priv.h>
#include <cpl_string.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>

namespace wrftools {
namespace {
constexpr const char* kCoordinateVariables[] = {"XLAT", "XLONG", "XLAT_M", "XLONG_M", "XLAT_U", "XLONG_U", "XLAT_V", "XLONG_V", "XLAT_C", "XLONG_C", "CLAT", "CLONG", "Times"};

std::string metadataValue(CSLConstList metadata, const char* key) {
    const char* value = CSLFetchNameValue(metadata, key);
    return value ? value : "";
}

std::string variableMetadata(CSLConstList metadata, const std::string& variable, const char* key) {
    const auto bare = metadataValue(metadata, key);
    if (!bare.empty()) return bare;
    return metadataValue(metadata, (variable + "#" + key).c_str());
}

int dimensionValueCount(const std::string& value) {
    const auto open = value.find('{'), close = value.rfind('}');
    const auto body = value.substr(open == std::string::npos ? 0 : open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
    if (body.empty()) return 0;
    return static_cast<int>(std::count(body.begin(), body.end(), ',')) + 1;
}

std::string globalValue(CSLConstList metadata, const char* key) {
    const auto prefixed = metadataValue(metadata, (std::string("NC_GLOBAL#") + key).c_str());
    return prefixed.empty() ? metadataValue(metadata, key) : prefixed;
}

// LU_INDEX's meaning for a *_LCZ_params.nc file (produced by the LCZ tab,
// or by w2w.py's own create_lcz_params_file) isn't captured by MMINLU
// alone: the base scheme's categories still apply to every non-urban
// pixel, but every urban pixel now carries a WUDAPT LCZ class number
// (31-40 for a 41-category target, 51-60 for 61) instead of the base
// scheme's own urban class - meaningless to look up in USGS/
// MODIFIED_IGBP_MODIS_NOAH's own table (USGS even has real entries at
// 31-33 of its own, "Low/High Intensity Residential"/"Industrial or
// Commercial", which a plain MMINLU lookup would show instead, actively
// wrong for an LCZ file).
//
// Detecting this from NUM_LAND_CAT alone isn't reliable: some real geo_em
// files already carry NUM_LAND_CAT=41 (and FLAG_URB_PARAM=1) from
// geogrid's own default (non-LCZ) urban physics scheme, with no LCZ
// classes in LU_INDEX at all (confirmed against w2w's own sample_data/
// geo_em.d04.nc, see PORT_W2W.MD Stage 3 - which is also why
// createLczExtentFile's OWN output for that exact fixture still carries
// NUM_LAND_CAT=41: create_lcz_extent_file sets it back to the ORIGINAL
// file's count, and that original file's own count already happened to be
// 41 before w2w ever touched it). Instead, key off THREE signals together:
// createLczParamsFile's own DESCRIPTION attribute (verbatim from w2w.py,
// see lcz.cpp) as the "produced by this pipeline" marker,
// NUM_LAND_CAT in {41, 61} for which offset to use, AND
// FLAG_URB_PARAM=1 - createLczExtentFile's own output copies DESCRIPTION
// forward (create_lcz_extent_file's dst_extent = dst_params.copy()) but
// always resets FLAG_URB_PARAM to 0 (it collapsed LCZ classes back to
// plain ISURBAN, so LU_INDEX genuinely has none left) - the one signal
// that reliably excludes it even in the NUM_LAND_CAT-coincidentally-
// already-41 case above. Only the actual *_LCZ_params.nc file passes all
// three checks.
std::string lczAwareCategoryScheme(CSLConstList metadata) {
    const auto scheme = globalValue(metadata, "MMINLU");
    const auto description = globalValue(metadata, "DESCRIPTION");
    if (description.find("W2W.py tool used to create geo_em") == std::string::npos) return scheme;
    if (globalValue(metadata, "FLAG_URB_PARAM") != "1") return scheme;
    const auto numLandCat = globalValue(metadata, "NUM_LAND_CAT");
    if (numLandCat == "41") return scheme + "+LCZ41";
    if (numLandCat == "61") return scheme + "+LCZ61";
    return scheme;
}

// Projection ids follow wrf-python/gis4wrf.core.constants.ProjectionTypes:
// 1=Lambert Conformal, 2=Polar Stereographic, 3=Mercator, 6=lat/lon.
Crs buildWrfCrs(CSLConstList metadata) {
    const auto projectionText = globalValue(metadata, "MAP_PROJ");
    if (projectionText.empty()) throw UserError("WRF/WPS file has no MAP_PROJ attribute.");
    int projection{};
    try { projection = std::stoi(projectionText); } catch (const std::exception&) { throw UserError("WRF/WPS file has an invalid MAP_PROJ attribute: " + projectionText); }
    const auto optionalReal = [&metadata](const char* key) -> std::optional<double> {
        const auto value = globalValue(metadata, key);
        return value.empty() ? std::nullopt : std::optional<double>(std::stod(value));
    };
    const double truelat1 = optionalReal("TRUELAT1").value_or(0.0);
    const double truelat2 = optionalReal("TRUELAT2").value_or(truelat1);
    const double standLon = optionalReal("STAND_LON").value_or(0.0);
    const double centerLat = optionalReal("MOAD_CEN_LAT").value_or(0.0);
    switch (projection) {
        case 1: return Crs::lambert(truelat1, truelat2, {standLon, centerLat});
        case 2: return Crs::polar(truelat1, standLon);
        case 3: return Crs::mercator(truelat1, standLon);
        case 6: {
            const auto poleLat = optionalReal("POLE_LAT"), poleLon = optionalReal("POLE_LON");
            if ((poleLat && *poleLat != 90.0) || (poleLon && *poleLon != 0.0))
                throw UnsupportedError("Geographic coordinate system with rotated pole is not supported.");
            return Crs::lonLat();
        }
        default: throw UnsupportedError("Unsupported WRF MAP_PROJ: " + std::to_string(projection));
    }
}

std::string subdatasetName(const char* entry) {
    const std::string value = entry ? entry : "";
    constexpr std::string_view suffix = "_NAME=";
    const auto namePosition = value.find(suffix);
    return namePosition == std::string::npos ? "" : value.substr(namePosition + suffix.size());
}

// The fully-qualified subdataset name for `variable` in `path`. Always
// forces GDAL's netCDF driver via the NETCDF: prefix rather than a bare
// path (see WrfFile's constructor comment) - some real production WRF
// output is NetCDF4/HDF5-backed, and a bare open lets GDAL's driver
// probing hand the file to its generic HDF5 driver instead, which names
// subdatasets HDF5:"path"://VAR and moves per-variable attributes like
// MemoryOrder off the subdataset's own metadata entirely.
std::string subdatasetTarget(const std::filesystem::path& path, const std::string& variable) {
    return "NETCDF:\"" + path.string() + "\":" + variable;
}

float coordinateValue(const std::filesystem::path& path, const char* variable, int x, int y) {
    const std::string target = subdatasetTarget(path, variable);
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(static_cast<GDALDataset*>(GDALOpenEx(target.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    if (!dataset) throw UserError("WRF coordinate variable is unavailable: " + std::string(variable));
    float value{};
    if (dataset->GetRasterBand(1)->RasterIO(GF_Read, x, y, 1, 1, &value, 1, 1, GDT_Float32, 0, 0) != CE_None) throw UserError("Could not read WRF coordinate variable.");
    return value;
}

// Top-down geotransform (x_min, dx, 0, y_max, 0, -dy), derived from the
// staggered U/V coordinate grids (edge-based) rather than the mass grid
// (cell-centered - using it would offset the raster by half a cell). GDAL's
// classic API returns netCDF arrays top-down: row 0 is the northernmost row.
std::array<double, 6> buildGeoTransform(const std::filesystem::path& path, const Crs& crs, int nx, int ny) {
    const auto swU = crs.toXy({coordinateValue(path, "XLONG_U", 0, ny - 1), coordinateValue(path, "XLAT_U", 0, ny - 1)});
    const auto seU = crs.toXy({coordinateValue(path, "XLONG_U", nx, ny - 1), coordinateValue(path, "XLAT_U", nx, ny - 1)});
    const auto swV = crs.toXy({coordinateValue(path, "XLONG_V", 0, ny), coordinateValue(path, "XLAT_V", 0, ny)});
    const auto nwV = crs.toXy({coordinateValue(path, "XLONG_V", 0, 0), coordinateValue(path, "XLAT_V", 0, 0)});
    const double dx = (seU.x - swU.x) / nx;
    const double dy = (nwV.y - swV.y) / ny;
    return {swU.x, dx, 0.0, nwV.y, 0.0, -dy};
}
}

WrfFile::WrfFile(std::filesystem::path path)
    : path_(std::move(path)), dataset_(nullptr, [](GDALDataset* value) { if (value) GDALClose(value); }) {
    GDALAllRegister();
    // NETCDF: forces GDAL's netCDF driver rather than letting it probe and
    // possibly pick its generic HDF5 driver for NetCDF4/HDF5-backed files -
    // see subdatasetTarget's comment. A file the netCDF driver genuinely
    // can't read (e.g. a GeoTIFF) fails to open here, which is still
    // reported as the same UserError below.
    const std::string containerTarget = "NETCDF:\"" + path_.string() + "\"";
    auto* raw = static_cast<GDALDataset*>(GDALOpenEx(containerTarget.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (!raw) throw UserError("Could not open WRF/WPS NetCDF file: " + path_.string());
    dataset_.reset(raw);
    const auto crs = buildWrfCrs(dataset_->GetMetadata());
    projectionWkt_ = crs.wkt();

    std::set<std::string> knownCoordinates(std::begin(kCoordinateVariables), std::end(kCoordinateVariables));
    CSLConstList subdatasets = dataset_->GetMetadata("SUBDATASETS");
    for (int i = 0; subdatasets && subdatasets[i]; ++i) {
        const std::string target = subdatasetName(subdatasets[i]);
        if (target.empty()) continue;
        const auto finalColon = target.rfind(':');
        const std::string name = finalColon == std::string::npos ? target : target.substr(finalColon + 1);
        if (knownCoordinates.contains(name)) continue;
        std::unique_ptr<GDALDataset, decltype(&GDALClose)> field(
            static_cast<GDALDataset*>(GDALOpenEx(target.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
        if (!field || field->GetRasterCount() == 0) continue;
        const auto memoryOrder = variableMetadata(field->GetMetadata(), name, "MemoryOrder");
        // GDAL exposes vertical-only arrays as narrow rasters; only WRF's XY
        // fields can be located and rendered on the mass grid.
        if (memoryOrder.rfind("XY", 0) != 0) continue;
        WrfVariable variable;
        variable.name = name;
        variable.description = variableMetadata(field->GetMetadata(), name, "description");
        variable.units = variableMetadata(field->GetMetadata(), name, "units");
        const auto dimensions = metadataValue(field->GetMetadata(), "NETCDF_DIM_EXTRA");
        const auto firstExtra = dimensions.find_first_not_of("{Time,}");
        if (firstExtra != std::string::npos) {
            const auto end = dimensions.find_first_of(",}", firstExtra);
            variable.extraDimension = dimensions.substr(firstExtra, end - firstExtra);
        }
        variable.timeCount = std::max(1, dimensionValueCount(metadataValue(field->GetMetadata(), "NETCDF_DIM_Time_VALUES")));
        variable.levelCount = variable.extraDimension ? std::max(1, field->GetRasterCount() / variable.timeCount) : 1;
        if (name == "LU_INDEX" || name == "IVGTYP") variable.categoryScheme = lczAwareCategoryScheme(dataset_->GetMetadata());
        else if (name == "ISLTYP" || name == "SCT_DOM" || name == "SCB_DOM" || variable.units == "category") variable.categoryScheme = "";
        variables_.push_back(std::move(variable));
    }
    if (variables_.empty()) throw UserError("No displayable WRF/WPS variables were found: " + path_.string());
    int width = std::numeric_limits<int>::max(), height = std::numeric_limits<int>::max();
    for (const auto& variable : variables_) {
        const std::string target = subdatasetTarget(path_, variable.name);
        std::unique_ptr<GDALDataset, decltype(&GDALClose)> field(static_cast<GDALDataset*>(GDALOpenEx(target.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
        width = std::min(width, field->GetRasterXSize());
        height = std::min(height, field->GetRasterYSize());
    }
    size_ = {width, height};
    geotransform_ = buildGeoTransform(path_, crs, width, height);
    // Geographic bounds for camera framing only (pixel placement uses the
    // projected geotransform above via the warp pipeline, not this box):
    // the four projected raster corners transformed to lon/lat, so a
    // rotated Lambert/Polar domain still frames correctly even though its
    // true outline isn't an axis-aligned lon/lat rectangle.
    {
        const auto xMin = geotransform_[0], yMax = geotransform_[3];
        const auto xMax = xMin + geotransform_[1] * width, yMin = yMax + geotransform_[5] * height;
        const std::array<LonLat, 4> corners{crs.toLonLat({xMin, yMin}), crs.toLonLat({xMax, yMin}), crs.toLonLat({xMin, yMax}), crs.toLonLat({xMax, yMax})};
        geographicBounds_ = {.west = corners[0].lon, .south = corners[0].lat, .east = corners[0].lon, .north = corners[0].lat};
        for (const auto& corner : corners) {
            geographicBounds_.west = std::min(geographicBounds_.west, corner.lon); geographicBounds_.east = std::max(geographicBounds_.east, corner.lon);
            geographicBounds_.south = std::min(geographicBounds_.south, corner.lat); geographicBounds_.north = std::max(geographicBounds_.north, corner.lat);
        }
    }
    for (const auto& variable : variables_) {
        const std::string target = subdatasetTarget(path_, variable.name);
        std::unique_ptr<GDALDataset, decltype(&GDALClose)> field(static_cast<GDALDataset*>(GDALOpenEx(target.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
        if (field->GetRasterXSize() == width + 1 && field->GetRasterYSize() == height) staggerAxis_[variable.name] = 1;
        else if (field->GetRasterYSize() == height + 1 && field->GetRasterXSize() == width) staggerAxis_[variable.name] = 0;
    }
    std::sort(variables_.begin(), variables_.end(), [](const WrfVariable& left, const WrfVariable& right) { return left.name < right.name; });
}

std::vector<float> WrfFile::read(const std::string& variable, int timeIndex, int levelIndex) const {
    const auto info = std::find_if(variables_.begin(), variables_.end(), [&variable](const WrfVariable& value) { return value.name == variable; });
    if (info == variables_.end() || timeIndex < 0 || levelIndex < 0 || timeIndex >= info->timeCount || levelIndex >= info->levelCount)
        throw UserError("Variable or time/level index is not available: " + variable);
    const int band = timeIndex * info->levelCount + levelIndex + 1;
    const std::string target = subdatasetTarget(path_, variable);
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> field(
        static_cast<GDALDataset*>(GDALOpenEx(target.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    if (!field || band > field->GetRasterCount()) throw UserError("Variable or timestep is not available: " + variable);
    const int width = field->GetRasterXSize(), height = field->GetRasterYSize();
    std::vector<float> values(static_cast<std::size_t>(width) * height);
    const auto result = field->GetRasterBand(band)->RasterIO(GF_Read, 0, 0, width, height, values.data(), width, height, GDT_Float32, 0, 0);
    if (result != CE_None) throw UserError("Could not read variable: " + variable);
    int hasNoData = false;
    const float noData = static_cast<float>(field->GetRasterBand(band)->GetNoDataValue(&hasNoData));
    constexpr float wrfFillValue = 9.9692099683868690e36f;
    for (auto& value : values) {
        if (std::abs(value - wrfFillValue) <= std::abs(wrfFillValue) * 1e-6f || (hasNoData && value == noData))
            value = std::numeric_limits<float>::quiet_NaN();
    }
    if (const auto staggered = staggerAxis_.find(variable); staggered != staggerAxis_.end()) {
        std::vector<float> destaggered;
        if (staggered->second == 1) {
            destaggered.resize(static_cast<std::size_t>(height) * (width - 1));
            for (int y = 0; y < height; ++y) for (int x = 0; x < width - 1; ++x) destaggered[static_cast<std::size_t>(y) * (width - 1) + x] = (values[static_cast<std::size_t>(y) * width + x] + values[static_cast<std::size_t>(y) * width + x + 1]) / 2.0f;
        } else {
            destaggered.resize(static_cast<std::size_t>(height - 1) * width);
            for (int y = 0; y < height - 1; ++y) for (int x = 0; x < width; ++x) destaggered[static_cast<std::size_t>(y) * width + x] = (values[static_cast<std::size_t>(y) * width + x] + values[static_cast<std::size_t>(y + 1) * width + x]) / 2.0f;
        }
        return destaggered;
    }
    return values;
}
}  // namespace wrftools
