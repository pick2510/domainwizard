#include "wrftools/file_extent.hpp"
#include "wrftools/error.hpp"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <algorithm>

namespace wrftools {

FileExtent readFileExtent(const std::filesystem::path& path) {
    GDALAllRegister();
    std::unique_ptr<GDALDataset, void (*)(GDALDataset*)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(path.string().c_str(), GDAL_OF_RASTER | GDAL_OF_VECTOR, nullptr, nullptr, nullptr)),
        [](GDALDataset* d) { if (d) GDALClose(d); });
    if (!dataset) throw UserError("Could not open " + path.string() + " as a raster or vector file");

    if (dataset->GetRasterCount() > 0) {
        double gt[6];
        if (dataset->GetGeoTransform(gt) != CE_None) throw UserError(path.string() + " has no geotransform.");
        const double width = dataset->GetRasterXSize(), height = dataset->GetRasterYSize();
        const double x0 = gt[0], y0 = gt[3];
        const double x1 = gt[0] + width * gt[1] + height * gt[2];
        const double y1 = gt[3] + width * gt[4] + height * gt[5];
        Bounds2D bounds{std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
        const auto* wkt = dataset->GetProjectionRef();
        if (!wkt || !*wkt) throw UserError(path.string() + " has no spatial reference defined.");
        return {bounds, Crs::fromWkt(wkt)};
    }

    auto* layer = dataset->GetLayer(0);
    if (!layer) throw UserError(path.string() + " has no raster bands and no vector layers");
    OGREnvelope envelope;
    if (layer->GetExtent(&envelope) != OGRERR_NONE) throw UserError("Could not read the extent of " + path.string());
    auto* srs = layer->GetSpatialRef();
    if (!srs) throw UserError(path.string() + " has no spatial reference defined.");
    char* wkt = nullptr;
    srs->exportToWkt(&wkt);
    std::string wktString = wkt ? wkt : "";
    CPLFree(wkt);
    return {{envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY}, Crs::fromWkt(wktString)};
}

}  // namespace wrftools
