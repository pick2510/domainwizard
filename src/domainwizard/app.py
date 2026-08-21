import os
import sys

# Must run before any `osgeo`/`gis4wrf.core` import (including transitively,
# via domainform below): GDAL/PROJ locate their data files (projection
# definitions, proj.db) lazily on first use, based on these env vars. In a
# PyInstaller bundle there's no system GDAL/PROJ install to fall back to, so
# without this, CRS transforms - which this app's whole domain-geometry
# pipeline depends on - fail or silently misbehave. See build.sh for what
# gets bundled at these paths.
if getattr(sys, 'frozen', False):
    _bundle_dir = sys._MEIPASS  # type: ignore[attr-defined]
    os.environ.setdefault('GDAL_DATA', os.path.join(_bundle_dir, 'share', 'gdal'))
    os.environ.setdefault('PROJ_DATA', os.path.join(_bundle_dir, 'share', 'proj'))

from PyQt6.QtWidgets import QApplication, QMainWindow, QSplitter, QWidget

from domainwizard.tilemap import TileMapWidget
from domainwizard.domainform import DomainForm
from domainwizard.formhelpers import WhiteScroll
from domainwizard import errorhandling

# OpenStreetMap's standard tile server. The GIS4WRF QGIS plugin's basemap
# (gis4wrf/plugin/geo.py, add_default_basemap()) points at Stamen, whose tile
# servers have been shut down (Stamen tiles moved to Stadia Maps, which now
# requires an API key) - OSM's server is free and needs no key, only a
# descriptive User-Agent (set in tilemap.py), per
# https://operations.osmfoundation.org/policies/tiles/.
TILE_URL = 'https://tile.openstreetmap.org/{z}/{x}/{y}.png'
ATTRIBUTION = '(c) OpenStreetMap contributors'


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle('Domain Wizard')
        self.resize(1300, 800)

        self.map = TileMapWidget(TILE_URL, attribution=ATTRIBUTION)
        self.map.set_center(0.0, 20.0, zoom=2)

        self.domain_form = DomainForm(self.map)
        self.domain_form.setMinimumWidth(340)
        self.domain_form.setMaximumWidth(420)

        splitter = QSplitter()
        splitter.addWidget(WhiteScroll(self.domain_form))
        splitter.addWidget(self.map)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([360, 940])

        self.setCentralWidget(splitter)


def main() -> None:
    app = QApplication(sys.argv)
    errorhandling.install()
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
