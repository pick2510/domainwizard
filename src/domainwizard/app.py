import sys

from PyQt6.QtGui import QBrush, QColor, QPen
from PyQt6.QtWidgets import QApplication, QMainWindow

from domainwizard.tilemap import Overlay, TileMapWidget

# The GIS4WRF QGIS plugin's basemap (gis4wrf/plugin/geo.py, add_default_basemap())
# points at Stamen's tile server, which has been shut down (Stamen tiles moved to
# Stadia Maps, which now requires an API key). Using OpenStreetMap's standard tile
# server here instead, which is free and requires no key - only a proper User-Agent
# (see tilemap.py), per https://operations.osmfoundation.org/policies/tiles/.
TILE_URL = 'https://tile.openstreetmap.org/{z}/{x}/{y}.png'
ATTRIBUTION = '(c) OpenStreetMap contributors'


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle('Domain Wizard')
        self.resize(1000, 700)

        self.map = TileMapWidget(TILE_URL, attribution=ATTRIBUTION)
        self.setCentralWidget(self.map)

        # Demo overlay: a rectangle roughly over the continental US, to
        # sanity-check the lon/lat -> screen projection and overlay drawing.
        outline = Overlay(
            rings=[[
                (-104.0, 32.0), (-90.0, 32.0), (-90.0, 42.0), (-104.0, 42.0), (-104.0, 32.0),
            ]],
            pen=QPen(QColor(255, 0, 0), 2),
            brush=QBrush(QColor(255, 0, 0, 40)),
        )
        self.map.set_overlays([outline])
        self.map.fit_bounds(-104.0, 32.0, -90.0, 42.0, padding_frac=0.2)


def main() -> None:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
