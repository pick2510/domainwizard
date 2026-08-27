#include "wrftools/warp.hpp"
#include "wrftools/error.hpp"

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <gdal_alg.h>
#include <gdalwarper.h>
#include <cpl_string.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace wrftools {
namespace {
constexpr float kNodataSentinel = -9999.0f;

// Above this, cap the warped output's larger dimension rather than let
// GDALWarp pick its own "preserve native resolution" target size: a
// several-arc-second global WPS_GEOG dataset (e.g. GMTED2010's
// 43200x21600) is orders of magnitude more detail than any screen can show,
// and Web Mercator's latitude stretching inflates the natural target size
// further still near the poles - unbounded, this made warping a real global
// dataset take minutes and gigabytes rather than seconds, not just look
// wrong. Large enough that every existing (much smaller) raster - a WRF
// domain, a regional WPS_GEOG dataset - warps at its native resolution
// exactly as before.
constexpr int kMaxWarpDimension = 4096;

// Cheap (no resampling) query for the pixel size GDALWarp would naturally
// pick for a plain "-t_srs EPSG:3857" warp with no -ts/-tr override -
// mirrors what the gdalwarp CLI itself computes internally before running
// the actual warp. Returns false (leaving outWidth/outHeight untouched) if
// GDAL can't suggest one, in which case the caller falls back to letting
// GDALWarp decide on its own.
bool suggestedWarpSize(GDALDatasetH source, int& outWidth, int& outHeight) {
    OGRSpatialReference targetSrs;
    targetSrs.importFromEPSG(3857);
    targetSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* targetWkt = nullptr;
    if (targetSrs.exportToWkt(&targetWkt) != OGRERR_NONE || !targetWkt) return false;
    void* transformer = GDALCreateGenImgProjTransformer(source, nullptr, nullptr, targetWkt, FALSE, 0.0, 1);
    CPLFree(targetWkt);
    if (!transformer) return false;
    double geoTransform[6];
    const bool ok = GDALSuggestedWarpOutput(source, GDALGenImgProjTransform, transformer, geoTransform, &outWidth, &outHeight) == CE_None;
    GDALDestroyGenImgProjTransformer(transformer);
    return ok;
}
}

WarpedRaster warpToWebMercator(std::span<const float> values, int width, int height,
    const std::string& sourceWkt, const std::array<double, 6>& sourceGeotransform) {
    GDALAllRegister();
    auto* memDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!memDriver) throw UserError("GDAL's MEM driver is unavailable.");

    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> source(
        memDriver->Create("", width, height, 1, GDT_Float32, nullptr), [](GDALDataset* d) { if (d) GDALClose(d); });
    if (!source) throw UserError("Could not create an in-memory raster for warping.");
    source->SetProjection(sourceWkt.c_str());
    source->SetGeoTransform(const_cast<double*>(sourceGeotransform.data()));

    std::vector<float> filled(values.begin(), values.end());
    for (auto& value : filled) if (!std::isfinite(value)) value = kNodataSentinel;
    auto* band = source->GetRasterBand(1);
    if (band->RasterIO(GF_Write, 0, 0, width, height, filled.data(), width, height, GDT_Float32, 0, 0) != CE_None)
        throw UserError("Could not write source raster for warping.");
    band->SetNoDataValue(kNodataSentinel);

    GDALDatasetH sourceHandle = GDALDataset::ToHandle(source.get());

    // Cap the target size up front - via -ts, so GDALWarp resamples
    // straight to the capped resolution in one pass - rather than warping
    // at whatever native-ish resolution it would otherwise choose and
    // discarding the excess after the fact.
    std::string targetWidthText, targetHeightText;
    int suggestedWidth = 0, suggestedHeight = 0;
    std::vector<const char*> argv{"-of", "MEM", "-t_srs", "EPSG:3857", "-r", "bilinear"};
    if (suggestedWarpSize(sourceHandle, suggestedWidth, suggestedHeight) && std::max(suggestedWidth, suggestedHeight) > kMaxWarpDimension) {
        const double scale = static_cast<double>(kMaxWarpDimension) / std::max(suggestedWidth, suggestedHeight);
        targetWidthText = std::to_string(std::max(1, static_cast<int>(std::lround(suggestedWidth * scale))));
        targetHeightText = std::to_string(std::max(1, static_cast<int>(std::lround(suggestedHeight * scale))));
        argv.insert(argv.end(), {"-ts", targetWidthText.c_str(), targetHeightText.c_str()});
    }
    argv.push_back(nullptr);
    std::unique_ptr<GDALWarpAppOptions, void (*)(GDALWarpAppOptions*)> options(
        GDALWarpAppOptionsNew(const_cast<char**>(argv.data()), nullptr), GDALWarpAppOptionsFree);
    if (!options) throw UserError("Could not build GDAL warp options.");

    int usageError = 0;
    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> warped(
        GDALDataset::FromHandle(GDALWarp("", nullptr, 1, &sourceHandle, options.get(), &usageError)),
        [](GDALDataset* d) { if (d) GDALClose(d); });
    if (!warped) throw UserError("GDAL warp to EPSG:3857 failed.");

    const int warpedWidth = warped->GetRasterXSize(), warpedHeight = warped->GetRasterYSize();
    std::vector<float> warpedValues(static_cast<std::size_t>(warpedWidth) * warpedHeight);
    auto* warpedBand = warped->GetRasterBand(1);
    if (warpedBand->RasterIO(GF_Read, 0, 0, warpedWidth, warpedHeight, warpedValues.data(), warpedWidth, warpedHeight, GDT_Float32, 0, 0) != CE_None)
        throw UserError("Could not read warped raster.");
    int hasNoData = false;
    const float nodata = static_cast<float>(warpedBand->GetNoDataValue(&hasNoData));
    if (hasNoData) {
        // Bilinear resampling blends a true-nodata edge pixel with its valid
        // neighbour, so a thin border of values close to (but not exactly)
        // the sentinel can survive at the domain's true edge - a relative
        // closeness check, matching the Python reference's np.isclose.
        for (auto& value : warpedValues) if (std::abs(value - nodata) <= 1e-5f * std::max(1.0f, std::abs(nodata))) value = std::numeric_limits<float>::quiet_NaN();
    }

    double warpedTransform[6];
    warped->GetGeoTransform(warpedTransform);
    const double minX = warpedTransform[0], maxY = warpedTransform[3];
    const double maxX = minX + warpedTransform[1] * warpedWidth, minY = maxY + warpedTransform[5] * warpedHeight;
    return {std::move(warpedValues), warpedWidth, warpedHeight, {minX, minY, maxX, maxY}};
}

std::vector<float> warpToGrid(std::span<const float> values, int width, int height, const std::string& sourceWkt,
    const std::array<double, 6>& sourceGeotransform, const std::string& destWkt, const std::array<double, 6>& destGeotransform, int destWidth,
    int destHeight, ResampleMethod resampling) {
    GDALAllRegister();
    auto* memDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!memDriver) throw UserError("GDAL's MEM driver is unavailable.");

    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> source(
        memDriver->Create("", width, height, 1, GDT_Float32, nullptr), [](GDALDataset* d) { if (d) GDALClose(d); });
    if (!source) throw UserError("Could not create an in-memory source raster for warping.");
    source->SetProjection(sourceWkt.c_str());
    source->SetGeoTransform(const_cast<double*>(sourceGeotransform.data()));

    std::vector<float> filled(values.begin(), values.end());
    for (auto& value : filled) if (!std::isfinite(value)) value = kNodataSentinel;
    auto* sourceBand = source->GetRasterBand(1);
    if (sourceBand->RasterIO(GF_Write, 0, 0, width, height, filled.data(), width, height, GDT_Float32, 0, 0) != CE_None)
        throw UserError("Could not write source raster for warping.");
    sourceBand->SetNoDataValue(kNodataSentinel);

    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> dest(
        memDriver->Create("", destWidth, destHeight, 1, GDT_Float32, nullptr), [](GDALDataset* d) { if (d) GDALClose(d); });
    if (!dest) throw UserError("Could not create an in-memory destination raster for warping.");
    dest->SetProjection(destWkt.c_str());
    dest->SetGeoTransform(const_cast<double*>(destGeotransform.data()));
    auto* destBand = dest->GetRasterBand(1);
    destBand->SetNoDataValue(kNodataSentinel);
    // GDALReprojectImage only ever writes pixels the source actually
    // covers - pre-fill so any destination pixel outside the source's
    // extent (or masked as source nodata) reads back as nodata rather
    // than whatever MEM's default zero-fill would leave it as.
    if (destBand->Fill(kNodataSentinel) != CE_None) throw UserError("Could not initialize destination raster for warping.");

    const GDALResampleAlg algorithm = [&] {
        switch (resampling) {
            case ResampleMethod::Bilinear: return GRA_Bilinear;
            case ResampleMethod::Average: return GRA_Average;
            case ResampleMethod::Mode: return GRA_Mode;
            case ResampleMethod::Nearest: return GRA_NearestNeighbour;
        }
        return GRA_NearestNeighbour;
    }();

    if (GDALReprojectImage(GDALDataset::ToHandle(source.get()), sourceWkt.c_str(), GDALDataset::ToHandle(dest.get()), destWkt.c_str(), algorithm, 0.0,
            0.0, nullptr, nullptr, nullptr) != CE_None)
        throw UserError("GDAL reprojection to the destination grid failed.");

    std::vector<float> result(static_cast<std::size_t>(destWidth) * destHeight);
    if (destBand->RasterIO(GF_Read, 0, 0, destWidth, destHeight, result.data(), destWidth, destHeight, GDT_Float32, 0, 0) != CE_None)
        throw UserError("Could not read warped raster.");
    for (auto& value : result)
        if (std::abs(value - kNodataSentinel) <= 1e-5f * std::max(1.0f, std::abs(kNodataSentinel))) value = std::numeric_limits<float>::quiet_NaN();
    return result;
}

}  // namespace wrftools
