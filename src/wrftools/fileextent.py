"""Reads the extent and CRS of a raster or vector file via GDAL/OGR directly.

Replaces the QGIS-plugin's "Set to Active Layer Extent" button, which relied
on QGIS's layer list (iface.activeLayer()). There's no such concept outside
QGIS, so instead the user picks a file and we read its extent with GDAL/OGR.
"""

from typing import Tuple

from osgeo import gdal, ogr, osr

from gis4wrf.core import BoundingBox2D


def read_extent_and_srs(path: str) -> Tuple[BoundingBox2D, osr.SpatialReference]:
    ds = gdal.OpenEx(path, gdal.OF_RASTER | gdal.OF_VECTOR)
    if ds is None:
        raise ValueError(f'Could not open {path} as a raster or vector file')

    if ds.RasterCount > 0:
        gt = ds.GetGeoTransform()
        width, height = ds.RasterXSize, ds.RasterYSize
        x0, y0 = gt[0], gt[3]
        x1 = gt[0] + width * gt[1] + height * gt[2]
        y1 = gt[3] + width * gt[4] + height * gt[5]
        bbox = BoundingBox2D(minx=min(x0, x1), maxx=max(x0, x1), miny=min(y0, y1), maxy=max(y0, y1))
        srs = osr.SpatialReference()
        srs.ImportFromWkt(ds.GetProjection())
        return bbox, srs

    layer = ds.GetLayer(0)
    if layer is None:
        raise ValueError(f'{path} has no raster bands and no vector layers')
    xmin, xmax, ymin, ymax = layer.GetExtent()
    bbox = BoundingBox2D(minx=xmin, maxx=xmax, miny=ymin, maxy=ymax)
    srs = layer.GetSpatialRef()
    if srs is None:
        raise ValueError(f'{path} has no spatial reference defined')
    return bbox, srs
