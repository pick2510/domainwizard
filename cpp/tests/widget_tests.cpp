#include "wrftools/tile_map_widget.hpp"
#include "wrftools/colorbar.hpp"
#include "wrftools/colormaps.hpp"
#include "wrftools/raster_layer.hpp"

#include <map>
#include <vector>

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
    // Bounds in EPSG:3857 metres (see tile_map_widget.hpp) - roughly the
    // Mercator projection of the 8.535-8.545E, 47.365-47.375N test box.
    map.setRasterOverlayGroup("view-rasters", {{raster, {950'517.0, 5'988'383.0, 951'631.0, 5'990'297.0}, 0.8, true}});
    map.setLegend(wrftools::buildColorbar("T2 (K)", 280, 300, wrftools::colormap("viridis")));
    map.setVectorOverlayGroup("domains", {{{{8.53, 47.36}, {8.55, 47.36}, {8.55, 47.38}, {8.53, 47.38}, {8.53, 47.36}}, Qt::red, 2.0}});
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

TEST_CASE("native colorbar formats ticks per the requested style") {
    const auto fixed = wrftools::buildColorbar("T2 (K)", 0, 1, wrftools::colormap("viridis"), 2, "fixed", 1);
    const auto scientific = wrftools::buildColorbar("T2 (K)", 0, 1, wrftools::colormap("viridis"), 2, "scientific", 1);
    CHECK_FALSE(fixed.isNull());
    CHECK_FALSE(scientific.isNull());
    // Fixed geometry regardless of tick style - see colorbar.hpp.
    CHECK(fixed.size() == scientific.size());
}

TEST_CASE("native colorbar varies with colormap, range, and tick count") {
    const auto viridis = wrftools::buildColorbar("T2 (K)", 0, 10, wrftools::colormap("viridis"));
    const auto plasma = wrftools::buildColorbar("T2 (K)", 0, 10, wrftools::colormap("plasma"));
    CHECK(viridis.toImage() != plasma.toImage());

    const auto narrow = wrftools::buildColorbar("T2 (K)", 0, 10, wrftools::colormap("viridis"));
    const auto wide = wrftools::buildColorbar("T2 (K)", 0, 100, wrftools::colormap("viridis"));
    CHECK(narrow.toImage() != wide.toImage());

    const auto three = wrftools::buildColorbar("T2 (K)", 0, 10, wrftools::colormap("viridis"), 3);
    const auto seven = wrftools::buildColorbar("T2 (K)", 0, 10, wrftools::colormap("viridis"), 7);
    CHECK(three.toImage() != seven.toImage());
}

TEST_CASE("categorical legend is a nonempty pixmap for known landuse values") {
    const auto legend = wrftools::categoricalLut("MODIFIED_IGBP_MODIS_NOAH", 1, 20);
    const auto pixmap = wrftools::buildCategoricalLegend(legend.lut, legend.labels, {1, 2, 17}, "LU_INDEX");
    CHECK_FALSE(pixmap.isNull());
    CHECK(pixmap.width() > 0);
    CHECK(pixmap.height() > 0);
}

TEST_CASE("categorical legend grows with row count and caps at 20 plus a summary row") {
    const auto legend = wrftools::categoricalLut("USGS", 1, 3);
    const std::map<int, std::string> labels{{1, "a"}, {2, "b"}, {3, "c"}};
    const auto full = wrftools::buildCategoricalLegend(legend.lut, labels, {1, 2, 3}, "LU_INDEX");
    const auto capped = wrftools::buildCategoricalLegend(legend.lut, labels, {1, 2}, "LU_INDEX");
    CHECK_FALSE(full.isNull());
    CHECK(full.height() > capped.height());

    std::vector<int> many(25);
    for (int i = 0; i < 25; ++i) many[static_cast<std::size_t>(i)] = i + 1;
    const auto overflowing = wrftools::buildCategoricalLegend(legend.lut, labels, many, "LU_INDEX");
    // 20 shown rows + one "+N more" row, one row taller than exactly 20.
    const auto exactlyTwenty = std::vector<int>(many.begin(), many.begin() + 20);
    const auto twentyRows = wrftools::buildCategoricalLegend(legend.lut, labels, exactlyTwenty, "LU_INDEX");
    CHECK(overflowing.height() == twentyRows.height() + 16);
}

TEST_CASE("rendered raster converts to a Qt image") {
    wrftools::RenderedRaster raster{{{255, 0, 0, 255}, {0, 255, 0, 255}}, 2, 1, 0, 1};
    const auto image = wrftools::rasterImage(raster);
    CHECK(image.size() == QSize(2, 1));
    CHECK(image.pixelColor(0, 0) == QColor(255, 0, 0, 255));
}
