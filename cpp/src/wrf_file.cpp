#include "wrftools/wrf_file.hpp"
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

std::string subdatasetName(const char* entry) {
    const std::string value = entry ? entry : "";
    constexpr std::string_view suffix = "_NAME=";
    const auto namePosition = value.find(suffix);
    return namePosition == std::string::npos ? "" : value.substr(namePosition + suffix.size());
}
}

WrfFile::WrfFile(std::filesystem::path path)
    : path_(std::move(path)), dataset_(nullptr, [](GDALDataset* value) { if (value) GDALClose(value); }) {
    GDALAllRegister();
    auto* raw = static_cast<GDALDataset*>(GDALOpenEx(path_.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (!raw) throw UserError("Could not open WRF/WPS NetCDF file: " + path_.string());
    dataset_.reset(raw);
    double transform[6];
    if (dataset_->GetGeoTransform(transform) == CE_None) std::copy(transform, transform + 6, geotransform_.begin());

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
        CSLConstList metadata = field->GetMetadata();
        const auto memoryOrder = variableMetadata(metadata, name, "MemoryOrder");
        // GDAL exposes vertical-only arrays as narrow rasters; only WRF's XY
        // fields can be located and rendered on the mass grid.
        if (memoryOrder.rfind("XY", 0) != 0) continue;
        WrfVariable variable;
        variable.name = name;
        variable.description = variableMetadata(metadata, name, "description");
        variable.units = variableMetadata(metadata, name, "units");
        const auto dimensions = metadataValue(metadata, "NETCDF_DIM_EXTRA");
        const auto firstExtra = dimensions.find_first_not_of("{Time,}");
        if (firstExtra != std::string::npos) {
            const auto end = dimensions.find_first_of(",}", firstExtra);
            variable.extraDimension = dimensions.substr(firstExtra, end - firstExtra);
        }
        variable.timeCount = std::max(1, dimensionValueCount(metadataValue(metadata, "NETCDF_DIM_Time_VALUES")));
        variable.levelCount = variable.extraDimension ? std::max(1, field->GetRasterCount() / variable.timeCount) : 1;
        if (name == "LU_INDEX" || name == "IVGTYP") variable.categoryScheme = metadataValue(dataset_->GetMetadata(), "NC_GLOBAL#MMINLU");
        else if (name == "ISLTYP" || name == "SCT_DOM" || name == "SCB_DOM" || variable.units == "category") variable.categoryScheme = "";
        variables_.push_back(std::move(variable));
    }
    if (variables_.empty()) throw UserError("No displayable WRF/WPS variables were found: " + path_.string());
    int width = std::numeric_limits<int>::max(), height = std::numeric_limits<int>::max();
    for (const auto& variable : variables_) {
        const std::string target = "NETCDF:\"" + path_.string() + "\":" + variable.name;
        std::unique_ptr<GDALDataset, decltype(&GDALClose)> field(static_cast<GDALDataset*>(GDALOpenEx(target.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
        width = std::min(width, field->GetRasterXSize());
        height = std::min(height, field->GetRasterYSize());
    }
    size_ = {width, height};
    for (const auto& variable : variables_) {
        const std::string target = "NETCDF:\"" + path_.string() + "\":" + variable.name;
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
    const std::string target = "NETCDF:\"" + path_.string() + "\":" + variable;
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
