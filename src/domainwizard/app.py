import sys

from PyQt6.QtWidgets import QApplication, QMainWindow, QSplitter, QWidget

from domainwizard.tilemap import TileMapWidget
from domainwizard.domainform import DomainForm
from domainwizard.formhelpers import WhiteScroll

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
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
