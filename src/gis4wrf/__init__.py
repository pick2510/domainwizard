# Vendored from https://github.com/GIS4WRF/gis4wrf (gis4wrf/core only).
#
# This is a minimal package init, intentionally NOT the real gis4wrf/__init__.py
# from the QGIS plugin repo - that one wires up QGIS plugin bootstrapping
# (classFactory, bootstrap_with_ui) which needs `qgis` and pulls in extra
# packaging machinery (pkg_resources) that this standalone app has no use for.
# Only the `gis4wrf.core` subpackage (domain/CRS/namelist logic, no QGIS
# dependency) is vendored here.
