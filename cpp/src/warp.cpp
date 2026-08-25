#include "wrftools/warp.hpp"
#include "wrftools/error.hpp"

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_string.h>

#include <cmath>
#include <limits>
#include <memory>

namespace wrftools {
namespace {
constexpr float kNodataSentinel = -9999.0f;
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

    char* argv[] = {const_cast<char*>("-of"), const_cast<char*>("MEM"), const_cast<char*>("-t_srs"), const_cast<char*>("EPSG:3857"), const_cast<char*>("-r"), const_cast<char*>("bilinear"), nullptr};
    std::unique_ptr<GDALWarpAppOptions, void (*)(GDALWarpAppOptions*)> options(
        GDALWarpAppOptionsNew(argv, nullptr), GDALWarpAppOptionsFree);
    if (!options) throw UserError("Could not build GDAL warp options.");

    GDALDatasetH sourceHandle = GDALDataset::ToHandle(source.get());
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

}  // namespace wrftools
