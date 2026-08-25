#include "wrftools/tile_map_widget.hpp"
#include "wrftools/colorbar.hpp"
#include "wrftools/raster_layer.hpp"

#include <QApplication>
#include <QImage>
#include <QTemporaryDir>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}

TEST_CASE("native map exports a readable image without downloaded tiles") {
    wrftools::TileMapWidget map;
    map.resize(480, 320);
    map.setCenter(8.54, 47.37, 10);
    map.zoomToBounds({8.535, 47.365}, {8.545, 47.375});
    QImage raster(8, 8, QImage::Format_RGBA8888);
    raster.fill(QColor(255, 0, 0, 150));
    map.setRasterOverlays({{raster, {8.535, 47.365}, {8.545, 47.375}, 0.8, true}});
    map.setLegend(wrftools::buildColorbar("T2 (K)", 280, 300, wrftools::colormap("viridis")));
    map.setVectorOverlays({{{{8.53, 47.36}, {8.55, 47.36}, {8.55, 47.38}, {8.53, 47.38}, {8.53, 47.36}}, Qt::red, 2.0}});
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath("map.png");
    REQUIRE(map.exportImage(path));
    QImage image(path);
    CHECK_FALSE(image.isNull());
    CHECK(image.size() == QSize(480, 320));
}

TEST_CASE("native colorbar is a nonempty movable-overlay-ready pixmap") {
    const auto legend = wrftools::buildColorbar("T2 (K)", 280, 300, wrftools::colormap("viridis"), 5);
    CHECK_FALSE(legend.isNull());
    CHECK(legend.width() == 180);
    CHECK(legend.height() == 274);
}

TEST_CASE("rendered raster converts to a Qt image") {
    wrftools::RenderedRaster raster{{{255, 0, 0, 255}, {0, 255, 0, 255}}, 2, 1, 0, 1};
    const auto image = wrftools::rasterImage(raster);
    CHECK(image.size() == QSize(2, 1));
    CHECK(image.pixelColor(0, 0) == QColor(255, 0, 0, 255));
}
