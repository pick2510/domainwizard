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

from wrftools.tilemap import LonLat, Overlay

# geo.py's update_domain_outline_layers() used a fixed red-for-main/blue-for-
# parent scheme, which relied on there being exactly one "main" domain (the
# innermost, always domains[0] under the old leaf-first storage) and a single
# linear chain of parents. Neither holds any more (Phase 1 of
# PLAN_TREE_DOMAINS.md: domains[0] is now the root, and a project can have
# several sibling domains at once that all need to be told apart) - so
# instead every domain gets its own color from a fixed, cyclic palette,
# assigned by its stable WPS domain number rather than by position/depth.
_PALETTE = [
    QColor(220, 30, 30),    # red
    QColor(30, 90, 220),    # blue
    QColor(30, 160, 60),    # green
    QColor(230, 140, 20),   # orange
    QColor(150, 40, 190),   # purple
    QColor(20, 160, 160),   # teal
    QColor(190, 30, 130),   # magenta
    QColor(140, 120, 20),   # olive
]


def _pen_for_domain_number(domain_number: int) -> QPen:
    return QPen(_PALETTE[(domain_number - 1) % len(_PALETTE)], 2)


def _wgs84_srs() -> osr.SpatialReference:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return srs


def compute_domain_overlays(project: 'core.Project') -> List[Overlay]:
    """Returns one Overlay (outline) per domain, in lon/lat, ready to hand to
    TileMapWidget.set_overlay_group('domains', ...)."""
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

        # project.bboxes() (which convert_project_to_gdal_outlines() reads)
        # returns domains in project.data['domains'] order, i.e. one feature
        # per domain in WPS domain-number order (idx 0 = domain 1 = root).
        pen = _pen_for_domain_number(idx + 1)
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
