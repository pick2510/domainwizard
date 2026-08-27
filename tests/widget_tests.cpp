#include "wrftools/tile_map_widget.hpp"
#include "wrftools/colorbar.hpp"
#include "wrftools/colormaps.hpp"
#include "wrftools/raster_layer.hpp"
#include "wrftools/theme.hpp"

#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QMouseEvent>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QTemporaryDir>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    const int result = Catch::Session().run(argc, argv);
    // quick_exit skips static-destructor/atexit teardown (Qt, GDAL/netCDF's
    // own driver-unregistration hooks, ...) - on Windows that teardown path
    // has been observed to hang indefinitely well after every test has
    // already run and reported its result, turning a passing run into a
    // ctest --timeout kill. The test process's own state doesn't need to
    // survive past this point, so skipping it is safe here even though it
    // wouldn't be in the shipped app.
    std::quick_exit(result);
}

namespace {
// QWidget::event() is protected, but QApplication::sendEvent dispatches to
// it internally regardless of the caller's access - the standard way to
// drive mouse handlers from outside a QWidget subclass.
void press(wrftools::TileMapWidget& map, QPointF pos) {
    QMouseEvent event(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&map, &event);
}
void move(wrftools::TileMapWidget& map, QPointF pos) {
    QMouseEvent event(QEvent::MouseMove, pos, pos, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&map, &event);
}
void release(wrftools::TileMapWidget& map, QPointF pos) {
    QMouseEvent event(QEvent::MouseButtonRelease, pos, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&map, &event);
}
}  // namespace

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

TEST_CASE("exporting to an unwritable path returns false") {
    wrftools::TileMapWidget map;
    map.resize(300, 200);
    CHECK_FALSE(map.exportImage("/no/such/directory/map.png"));
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

TEST_CASE("overlay groups are independent") {
    wrftools::TileMapWidget map;
    map.setVectorOverlayGroup("domains", {{{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}}, Qt::red, 2.0}});
    CHECK(map.vectorOverlayGroupSize("domains") == 1);
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 0);

    QImage image(2, 2, QImage::Format_RGBA8888);
    map.setRasterOverlayGroup("view-rasters", {{image, {0, 0, 1, 1}}});

    CHECK(map.vectorOverlayGroupSize("domains") == 1);
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 1);

    map.clearVectorOverlayGroup("domains");
    CHECK_FALSE(map.hasVectorOverlayGroup("domains"));
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 1);
}

TEST_CASE("legend defaults to none and can be set and cleared") {
    wrftools::TileMapWidget map;
    CHECK_FALSE(map.hasLegend());
    map.setLegend(QPixmap(10, 10));
    CHECK(map.hasLegend());
    map.setLegend(QPixmap());
    CHECK_FALSE(map.hasLegend());
}

TEST_CASE("info text defaults to none and can be set and cleared") {
    wrftools::TileMapWidget map;
    CHECK_FALSE(map.hasInfoText());
    map.setInfoText("T2 (degC) - 2020-01-01 00:30");
    CHECK(map.hasInfoText());
    map.setInfoText("");
    CHECK_FALSE(map.hasInfoText());
}

TEST_CASE("legend defaults to the top-right corner") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setLegend(QPixmap(60, 30));
    map.grab();  // forces a real paintEvent, populating legendRect()
    CHECK_FALSE(map.legendPosition().has_value());
    CHECK(map.legendRect().right() == Catch::Approx(map.width() - 10).margin(1));
    CHECK(map.legendRect().top() == Catch::Approx(10).margin(1));
}

TEST_CASE("info text defaults to the top-left corner") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setInfoText("T2 (degC)");
    map.grab();
    CHECK_FALSE(map.infoPosition().has_value());
    CHECK(map.infoRect().left() == Catch::Approx(10).margin(1));
    CHECK(map.infoRect().top() == Catch::Approx(10).margin(1));
}

TEST_CASE("dragging the legend moves it and does not pan the map") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(10.0, 20.0, 4);
    map.setLegend(QPixmap(60, 30));
    map.grab();

    const auto center = map.legendRect().center();
    const auto offset = center - map.legendRect().topLeft();
    press(map, center);
    CHECK(map.dragTarget() == "legend");
    move(map, QPointF(20, 250));
    map.grab();
    REQUIRE(map.legendPosition().has_value());
    CHECK(*map.legendPosition() == QPointF(20, 250) - offset);
    release(map, QPointF(20, 250));
    CHECK(map.dragTarget().isEmpty());

    CHECK(map.centerLongitude() == Catch::Approx(10.0));
    CHECK(map.centerLatitude() == Catch::Approx(20.0));
}

TEST_CASE("legend defaults to unscaled and exposes a resize handle at its corner") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setLegend(QPixmap(60, 30));
    map.grab();
    CHECK(map.legendScale() == Catch::Approx(1.0));
    CHECK(map.legendResizeHandleRect().right() == Catch::Approx(map.legendRect().right()).margin(1));
    CHECK(map.legendResizeHandleRect().bottom() == Catch::Approx(map.legendRect().bottom()).margin(1));
}

TEST_CASE("dragging the legend's resize handle grows it and does not move or pan it") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(10.0, 20.0, 4);
    map.setLegend(QPixmap(60, 30));
    map.grab();

    // legendPosition_ is unset, so the legend stays anchored to the
    // top-right corner (movableRect's default) as it grows - only its
    // right/top edges, not its top-left, stay put.
    const auto originalRight = map.legendRect().right();
    const auto originalTop = map.legendRect().top();
    const auto handle = map.legendResizeHandleRect().center();
    press(map, handle);
    CHECK(map.dragTarget() == "legend-resize");
    move(map, handle + QPointF(60, 30));  // drag outward by one legend-width
    map.grab();
    CHECK(map.legendScale() > 1.5);
    release(map, handle + QPointF(60, 30));
    CHECK(map.dragTarget().isEmpty());

    map.grab();
    CHECK(map.legendRect().right() == Catch::Approx(originalRight).margin(1));
    CHECK(map.legendRect().top() == Catch::Approx(originalTop).margin(1));
    CHECK(map.centerLongitude() == Catch::Approx(10.0));
    CHECK(map.centerLatitude() == Catch::Approx(20.0));
}

TEST_CASE("shrinking the legend below the minimum scale clamps rather than inverting") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setLegend(QPixmap(60, 30));
    map.grab();

    const auto handle = map.legendResizeHandleRect().center();
    press(map, handle);
    move(map, map.legendRect().topLeft());  // drag all the way back to the anchor
    map.grab();
    CHECK(map.legendScale() == Catch::Approx(0.4));
    release(map, map.legendRect().topLeft());
}

TEST_CASE("dragging the info overlay moves it independently of the legend") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setLegend(QPixmap(60, 30));
    map.setInfoText("T2 (degC)");
    map.grab();

    const auto legendPosBefore = map.legendPosition();
    const auto center = map.infoRect().center();
    press(map, center);
    CHECK(map.dragTarget() == "info");
    move(map, QPointF(150, 150));
    map.grab();
    release(map, QPointF(150, 150));

    CHECK(map.infoPosition().has_value());
    CHECK(map.legendPosition() == legendPosBefore);  // untouched
}

TEST_CASE("a press inside a draggable overlay group's polygon starts an overlay drag, reports the hit index and lon/lat") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(0.0, 0.0, 6);
    map.setVectorOverlayGroup("domains", {{{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}}, Qt::red, 2.0, /*closed=*/true}});
    map.setDraggableVectorOverlayGroup("domains");

    std::optional<std::size_t> startedIndex;
    std::optional<wrftools::LonLat> startedLonLat;
    int moveCount = 0;
    bool ended = false;
    map.setOverlayDragHandlers(
        [&](std::size_t index, wrftools::LonLat lonLat) { startedIndex = index; startedLonLat = lonLat; },
        [&](std::size_t, wrftools::LonLat) { ++moveCount; },
        [&] { ended = true; });

    map.grab();  // populates real screen geometry for the polygon
    const QPointF widgetCenter(map.width() / 2.0, map.height() / 2.0);  // lon=0/lat=0, inside the polygon
    press(map, widgetCenter);
    CHECK(map.dragTarget() == "overlay");
    REQUIRE(startedIndex.has_value());
    CHECK(*startedIndex == 0);
    REQUIRE(startedLonLat.has_value());
    CHECK(startedLonLat->lon == Catch::Approx(0.0).margin(0.01));
    CHECK(startedLonLat->lat == Catch::Approx(0.0).margin(0.01));

    move(map, widgetCenter + QPointF(20, 15));
    CHECK(moveCount == 1);
    // The drag-highlight pass (a second, white-halo + black-dashed redraw
    // of the dragged polygon) must not crash mid-drag.
    CHECK_NOTHROW(map.grab());

    release(map, widgetCenter + QPointF(20, 15));
    CHECK(map.dragTarget().isEmpty());
    CHECK(ended);
}

TEST_CASE("a press outside every polygon in the draggable group falls through to panning") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(0.0, 0.0, 6);
    map.setVectorOverlayGroup("domains", {{{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}}, Qt::red, 2.0, /*closed=*/true}});
    map.setDraggableVectorOverlayGroup("domains");
    bool started = false;
    map.setOverlayDragHandlers([&](std::size_t, wrftools::LonLat) { started = true; }, {}, {});

    map.grab();
    press(map, QPointF(5, 5));  // far corner, well outside the polygon
    CHECK(map.dragTarget().isEmpty());
    CHECK_FALSE(started);
}

TEST_CASE("an empty draggable group name disables overlay dragging entirely") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(0.0, 0.0, 6);
    map.setVectorOverlayGroup("domains", {{{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}}, Qt::red, 2.0, /*closed=*/true}});
    // Never armed via setDraggableVectorOverlayGroup.
    bool started = false;
    map.setOverlayDragHandlers([&](std::size_t, wrftools::LonLat) { started = true; }, {}, {});

    map.grab();
    press(map, QPointF(map.width() / 2.0, map.height() / 2.0));
    CHECK(map.dragTarget().isEmpty());
    CHECK_FALSE(started);
}

TEST_CASE("pressing near a handle starts an overlay resize, taking priority over a body drag") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(0.0, 0.0, 6);
    // A square domain outline with its SW handle at (-1,-1) - the handle
    // sits exactly on the polygon's own boundary, which a body hit test
    // would also consider "inside" if it ran first.
    map.setVectorOverlayGroup("domains", {{{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}}, Qt::red, 2.0,
        /*closed=*/true, {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}}});
    map.setDraggableVectorOverlayGroup("domains");

    std::optional<std::size_t> startedOverlay, startedHandle;
    std::optional<wrftools::LonLat> startedLonLat;
    bool bodyDragStarted = false;
    int moveCount = 0;
    bool ended = false;
    map.setOverlayResizeHandlers(
        [&](std::size_t overlay, std::size_t handle, wrftools::LonLat lonLat) { startedOverlay = overlay; startedHandle = handle; startedLonLat = lonLat; },
        [&](std::size_t, std::size_t, wrftools::LonLat) { ++moveCount; },
        [&] { ended = true; });
    map.setOverlayDragHandlers([&](std::size_t, wrftools::LonLat) { bodyDragStarted = true; }, {}, {});

    map.grab();
    // The SW handle (lon=-1/lat=-1) at zoom 6 centered on (0,0) in a
    // 400x300 widget - computed directly from worldPixel()'s own formula
    // (Web Mercator tile math), not guessed, so the hit test's small pixel
    // radius isn't left to chance.
    const QPointF handleScreenPos(154.49, 195.51);
    press(map, handleScreenPos);
    CHECK(map.dragTarget() == "overlay-resize");
    CHECK_FALSE(bodyDragStarted);
    REQUIRE(startedOverlay.has_value());
    CHECK(*startedOverlay == 0);
    REQUIRE(startedHandle.has_value());
    CHECK(*startedHandle == 0);  // SW is index 0
    REQUIRE(startedLonLat.has_value());
    CHECK(startedLonLat->lon == Catch::Approx(-1.0).margin(0.05));
    CHECK(startedLonLat->lat == Catch::Approx(-1.0).margin(0.05));

    move(map, handleScreenPos + QPointF(-15, 15));
    CHECK(moveCount == 1);
    CHECK_NOTHROW(map.grab());  // handle squares + any in-progress highlight must not crash

    release(map, handleScreenPos + QPointF(-15, 15));
    CHECK(map.dragTarget().isEmpty());
    CHECK(ended);
}

TEST_CASE("a press away from every handle still starts a body drag when inside the polygon") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(0.0, 0.0, 6);
    map.setVectorOverlayGroup("domains", {{{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}}, Qt::red, 2.0,
        /*closed=*/true, {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}}});
    map.setDraggableVectorOverlayGroup("domains");
    bool resizeStarted = false, dragStarted = false;
    map.setOverlayResizeHandlers([&](std::size_t, std::size_t, wrftools::LonLat) { resizeStarted = true; }, {}, {});
    map.setOverlayDragHandlers([&](std::size_t, wrftools::LonLat) { dragStarted = true; }, {}, {});

    map.grab();
    press(map, QPointF(map.width() / 2.0, map.height() / 2.0));  // dead center, far from any corner
    CHECK(map.dragTarget() == "overlay");
    CHECK(dragStarted);
    CHECK_FALSE(resizeStarted);
}

TEST_CASE("dragging empty map area still pans as before") {
    wrftools::TileMapWidget map;
    map.resize(400, 300);
    map.setCenter(0.0, 0.0, 4);
    map.setLegend(QPixmap(60, 30));
    map.grab();

    press(map, QPointF(200, 150));  // far from the legend's top-right box
    CHECK(map.dragTarget().isEmpty());
    move(map, QPointF(150, 150));
    release(map, QPointF(150, 150));

    CHECK_FALSE((map.centerLongitude() == Catch::Approx(0.0) && map.centerLatitude() == Catch::Approx(0.0)));
}

TEST_CASE("map paints with both a raster and a vector overlay group populated") {
    wrftools::TileMapWidget map;
    map.resize(64, 64);
    map.setCenter(0.0, 0.0, 4);

    QImage image(4, 4, QImage::Format_RGBA8888);
    image.fill(QColor(255, 0, 0, 255));
    map.setRasterOverlayGroup("view-rasters", {{image, {-1000.0, -1000.0, 1000.0, 1000.0}, 0.5}});
    map.setVectorOverlayGroup("domains", {{{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}}, Qt::red, 2.0}});
    CHECK_NOTHROW(map.grab());  // forces a real paintEvent offscreen
}

TEST_CASE("raster overlay outside the viewport is skipped without error") {
    wrftools::TileMapWidget map;
    map.resize(64, 64);
    map.setCenter(0.0, 0.0, 4);
    QImage image(2, 2, QImage::Format_RGBA8888);
    map.setRasterOverlayGroup("view-rasters", {{image, {10'000'000.0, 10'000'000.0, 10'000'001.0, 10'000'001.0}}});
    CHECK_NOTHROW(map.grab());
}

TEST_CASE("the map exposes every built-in basemap provider, selectable via its combo box") {
    wrftools::TileMapWidget map;
    const auto& providers = map.tileProviders();
    REQUIRE(providers.size() == 29);
    CHECK(providers.front().name == "OpenStreetMap Standard");
    CHECK(providers.back().name == "Bing VirtualEarth");
    CHECK(map.currentTileProviderIndex() == 0);
    CHECK(map.currentTileProvider().name == "OpenStreetMap Standard");

    REQUIRE(map.tileProviderCombo() != nullptr);
    CHECK(map.tileProviderCombo()->count() == static_cast<int>(providers.size()));
    for (std::size_t i = 0; i < providers.size(); ++i) CHECK(map.tileProviderCombo()->itemText(static_cast<int>(i)) == providers[i].name);
}

TEST_CASE("selecting a provider in the combo box switches the active basemap") {
    wrftools::TileMapWidget map;
    const auto googleSatelliteIndex = map.tileProviderCombo()->findText("Google Satellite");
    REQUIRE(googleSatelliteIndex >= 0);

    map.tileProviderCombo()->setCurrentIndex(googleSatelliteIndex);
    CHECK(map.currentTileProviderIndex() == googleSatelliteIndex);
    CHECK(map.currentTileProvider().name == "Google Satellite");
    CHECK(map.currentTileProvider().url == "https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}");
}

TEST_CASE("setTileProvider switches the basemap directly and keeps the combo box in sync") {
    wrftools::TileMapWidget map;
    const auto osmHotIndex = map.tileProviderCombo()->findText("OpenStreetMap H.O.T.");
    REQUIRE(osmHotIndex >= 0);

    map.setTileProvider(osmHotIndex);
    CHECK(map.currentTileProviderIndex() == osmHotIndex);
    CHECK(map.tileProviderCombo()->currentIndex() == osmHotIndex);
}

TEST_CASE("an out-of-range provider index is ignored") {
    wrftools::TileMapWidget map;
    map.setTileProvider(static_cast<int>(map.tileProviders().size()));
    CHECK(map.currentTileProviderIndex() == 0);
    map.setTileProvider(-1);
    CHECK(map.currentTileProviderIndex() == 0);
}

TEST_CASE("exporting hides the provider combo box and restores it afterward") {
    wrftools::TileMapWidget map;
    map.resize(320, 240);
    map.show();
    REQUIRE(map.tileProviderCombo()->isVisible());

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    REQUIRE(map.exportImage(directory.filePath("map.png")));

    CHECK(map.tileProviderCombo()->isVisible());
}

TEST_CASE("exporting hides the combo box even if it started out hidden") {
    wrftools::TileMapWidget map;
    map.resize(320, 240);
    map.tileProviderCombo()->setVisible(false);

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    REQUIRE(map.exportImage(directory.filePath("map.png")));

    CHECK_FALSE(map.tileProviderCombo()->isVisible());
}

TEST_CASE("switching basemaps repaints without downloading any tiles") {
    wrftools::TileMapWidget map;
    map.resize(320, 240);
    map.setCenter(8.54, 47.37, 6);
    for (int i = 0; i < static_cast<int>(map.tileProviders().size()); ++i) {
        map.setTileProvider(i);
        CHECK_NOTHROW(map.grab());
    }
}

TEST_CASE("rendered raster converts to a Qt image") {
    wrftools::RenderedRaster raster{{{255, 0, 0, 255}, {0, 255, 0, 255}}, 2, 1, 0, 1};
    const auto image = wrftools::rasterImage(raster);
    CHECK(image.size() == QSize(2, 1));
    CHECK(image.pixelColor(0, 0) == QColor(255, 0, 0, 255));
}

TEST_CASE("darkPalette uses a genuinely dark window with light, readable text") {
    const auto palette = wrftools::darkPalette();
    CHECK(palette.color(QPalette::Window).lightness() < 100);
    CHECK(palette.color(QPalette::Base).lightness() < 100);
    CHECK(palette.color(QPalette::WindowText).lightness() > 150);
    CHECK(palette.color(QPalette::Text).lightness() > 150);
    CHECK(palette.color(QPalette::ButtonText).lightness() > 150);
    // Disabled text is dimmer than enabled text, not identical to it - a
    // manually-built palette that skips PaletteState::Disabled entirely
    // would otherwise leave every disabled widget looking enabled.
    CHECK(palette.color(QPalette::Disabled, QPalette::Text).lightness() < palette.color(QPalette::Text).lightness());
}

TEST_CASE("applyColorScheme switches to a dark Fusion palette for Dark and reverts otherwise") {
    REQUIRE(qApp);
    const auto darkWindowColor = wrftools::darkPalette().color(QPalette::Window);

    wrftools::applyColorScheme(*qApp, wrftools::ColorScheme::Dark);
    CHECK(qApp->style()->objectName().compare("fusion", Qt::CaseInsensitive) == 0);
    CHECK(qApp->palette().color(QPalette::Window) == darkWindowColor);

    wrftools::applyColorScheme(*qApp, wrftools::ColorScheme::Light);
    CHECK(qApp->palette().color(QPalette::Window) != darkWindowColor);

    // Back to dark, then Unknown (what an unthemed/offscreen platform
    // reports) - Unknown must not be treated as dark either.
    wrftools::applyColorScheme(*qApp, wrftools::ColorScheme::Dark);
    wrftools::applyColorScheme(*qApp, wrftools::ColorScheme::Unknown);
    CHECK(qApp->palette().color(QPalette::Window) != darkWindowColor);
}

TEST_CASE("themePreference defaults to System and round-trips through setThemePreference") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // An isolated IniFormat file, not the real per-user config store (see
    // theme.hpp: production code passes a default-constructed QSettings()
    // instead, using the app's own organization/application name).
    const auto path = dir.filePath("theme.ini");

    {
        QSettings settings(path, QSettings::IniFormat);
        CHECK(wrftools::themePreference(settings) == wrftools::ThemePreference::System);  // nothing saved yet
    }

    {
        QSettings settings(path, QSettings::IniFormat);
        wrftools::setThemePreference(settings, wrftools::ThemePreference::Dark);
    }
    {
        // A fresh QSettings instance over the same file, not the same
        // object - proves the choice was actually persisted to disk, not
        // just cached in memory.
        QSettings settings(path, QSettings::IniFormat);
        CHECK(wrftools::themePreference(settings) == wrftools::ThemePreference::Dark);
    }

    {
        QSettings settings(path, QSettings::IniFormat);
        wrftools::setThemePreference(settings, wrftools::ThemePreference::Light);
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        CHECK(wrftools::themePreference(settings) == wrftools::ThemePreference::Light);
    }

    {
        QSettings settings(path, QSettings::IniFormat);
        wrftools::setThemePreference(settings, wrftools::ThemePreference::System);
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        CHECK(wrftools::themePreference(settings) == wrftools::ThemePreference::System);
    }
}
