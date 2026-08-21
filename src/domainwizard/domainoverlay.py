"""Bridges gis4wrf.core's GDAL/OGR domain-outline generation to the
TileMapWidget's lon/lat overlay drawing.

This replaces gis4wrf/plugin/geo.py's update_domain_outline_layers(), which
built QgsVectorLayer objects for QGIS's canvas. The geometry generation
itself (gis4wrf.core.convert_project_to_gdal_outlines) is unchanged and
reused as-is - only the final reprojection-for-display and rendering step
differs, since there's no QGIS canvas to hand a native-CRS layer to.
"""

from typing import List, Optional, Tuple

from osgeo import osr
from PyQt6.QtGui import QBrush, QColor, QPen

import gis4wrf.core as core

from domainwizard.tilemap import LonLat, Overlay

# Domain 0 (the innermost/main domain) is drawn in red, matching geo.py's
# update_domain_outline_layers(); nested parent domains are drawn in blue.
MAIN_DOMAIN_PEN = QPen(QColor(220, 30, 30), 2)
PARENT_DOMAIN_PEN = QPen(QColor(30, 90, 220), 2)


def _wgs84_srs() -> osr.SpatialReference:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return srs


def compute_domain_overlays(project: 'core.Project') -> List[Overlay]:
    """Returns one Overlay (outline) per domain, in lon/lat, ready to hand to
    TileMapWidget.set_overlays()."""
    gdal_ds = core.convert_project_to_gdal_outlines(project)
    layer = gdal_ds.GetLayer(0)
    domain_srs = layer.GetSpatialRef()

    transform = osr.CoordinateTransformation(domain_srs, _wgs84_srs())

    overlays = []
    layer.ResetReading()
    for idx, feature in enumerate(layer):
        geom = feature.GetGeometryRef().Clone()

        # Densify before reprojecting: a straight line in the domain's
        # projected CRS is generally curved in lon/lat, so without extra
        # vertices along each edge the outline would render as a distorted
        # straight-edged polygon instead of following the true projection.
        ring = geom.GetGeometryRef(0)
        perimeter = ring.Length()
        if perimeter > 0:
            geom.Segmentize(perimeter / 200.0)

        geom.Transform(transform)
        ring = geom.GetGeometryRef(0)
        points: List[LonLat] = [(ring.GetX(i), ring.GetY(i)) for i in range(ring.GetPointCount())]

        # idx 0 is always the innermost/main domain (see project.py: bboxes()
        # returns domains in project.data['domains'] order, domains[0] first),
        # matching geo.py's update_domain_outline_layers() red/blue convention.
        pen = MAIN_DOMAIN_PEN if idx == 0 else PARENT_DOMAIN_PEN
        overlays.append(Overlay(rings=[points], pen=pen, brush=QBrush(), closed=True))

    return overlays


def domain_lonlat_bounds(project: 'core.Project') -> Optional[Tuple[float, float, float, float]]:
    """Returns (min_lon, min_lat, max_lon, max_lat) covering all domains, for
    TileMapWidget.fit_bounds()."""
    overlays = compute_domain_overlays(project)
    if not overlays:
        return None
    lons = [lon for overlay in overlays for ring in overlay.rings for lon, lat in ring]
    lats = [lat for overlay in overlays for ring in overlay.rings for lon, lat in ring]
    return min(lons), min(lats), max(lons), max(lats)
