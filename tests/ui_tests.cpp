// UI-level tests (real Qt widget interaction) for view_form/domain_form/main_window.
// Runs headless via QT_QPA_PLATFORM=offscreen, set by CMakeLists.txt for this target.
// WORKING_DIRECTORY is the repo root (also set by CMakeLists.txt), so
// fixtures resolve as "tests/fixtures/...".
#include "wrftools/colormaps.hpp"
#include "wrftools/domain_form.hpp"
#include "wrftools/domain_overlay.hpp"
#include "wrftools/error.hpp"
#include "wrftools/geotiff_convert_form.hpp"
#include "wrftools/main_window.hpp"
#include "wrftools/theme.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/view_form.hpp"
#include "wrftools/wps_namelist.hpp"
#include "fast_exit.hpp"

#include <gdal_priv.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QMouseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wrftools;

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    // MainWindow's theme menu persists Options > Theme via a
    // default-constructed QSettings(), which needs an organization/
    // application name to know where to read/write - give it one distinct
    // from the real app's ("WRF Tools"/"WRF Tools", set in main.cpp) so a
    // test run never touches a developer's actual persisted preference.
    application.setOrganizationName("WRF Tools Tests");
    application.setApplicationName("wrftools_ui_tests");
    const int result = Catch::Session().run(argc, argv);
    wrftools_tests::fastExit(result);  // see fast_exit.hpp
}

namespace {
// Pumps the event loop until a GeotiffConvertForm's background worker
// finishes (its completion runs via QMetaObject::invokeMethod, so it only
// happens while the loop is spinning) or 10s elapse, whichever is first -
// the fixtures used here convert in well under a second.
void waitWhileRunning(GeotiffConvertForm& form) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (form.isRunning() && std::chrono::steady_clock::now() < deadline) QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}
}  // namespace

TEST_CASE("native map widget constructs headlessly") {
    TileMapWidget map;
    map.resize(200, 200);
    CHECK(map.size().width() == 200);
}

TEST_CASE("domain form builds a tree matching the sibling namelist") {
    TileMapWidget map;
    DomainForm form(&map);
    form.setProject(readWpsNamelist("tests/fixtures/namelist_siblings.wps"));
    auto* tree = form.domainTree();
    REQUIRE(tree->topLevelItemCount() == 1);
    auto* root = tree->topLevelItem(0);
    REQUIRE(root->childCount() == 1);
    auto* domain2 = root->child(0);
    // domains 3 and 4 both nest directly under domain 2 (siblings) -
    // exactly the case that motivated the tree-structured domain model.
    REQUIRE(domain2->childCount() == 2);
    CHECK(form.project()->domains.domains()[2].parentId == 2);
    CHECK(form.project()->domains.domains()[3].parentId == 2);
}

TEST_CASE("view form opens a series as one file row with real timestamp labels") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_02_00_00.nc",
    });
    // Four files sharing one (kind, domain) group in one WRFFileSeries -
    // one row, not four.
    REQUIRE(form.fileTreeWidget()->topLevelItemCount() == 1);
}

TEST_CASE("view form builds a multi-layer stack that reorders and hides") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    REQUIRE(form.fileTreeWidget()->topLevelItemCount() == 1);

    form.addLayer();
    form.addLayer();
    REQUIRE(form.layers().size() == 2);
    // layers() is bottom-first (draw order); the tree displays topmost
    // first - see rebuildLayerTree.
    CHECK(form.layerTreeWidget()->topLevelItemCount() == 2);

    // Selecting the top row (the second/most-recently-added layer) and
    // hiding it via its checkbox is real widget interaction, matching
    // on_layer_item_changed in the Python reference.
    auto* topRow = form.layerTreeWidget()->topLevelItem(0);
    topRow->setCheckState(0, Qt::Unchecked);
    CHECK_FALSE(form.layers().back().settings.visible);
}

TEST_CASE("view form layers default to auto tick formatting") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();
    REQUIRE(form.layers().size() == 1);
    const auto& settings = form.layers().front().settings;
    CHECK(settings.tickCount == 3);
    CHECK(settings.tickFormat == "auto");
    CHECK(settings.tickDecimals == 2);
}

// --- test_ui_view_layers.py port --------------------------------------------
// ViewForm::openFiles() is the entry point both the real "Open…" button and
// these tests use (see its own doc comment) - no QFileDialog monkeypatching
// needed, unlike the Python reference.

TEST_CASE("opening a file and adding a layer") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    CHECK(form.fileTreeWidget()->topLevelItemCount() == 1);

    form.addLayer();
    CHECK(form.layers().size() == 1);
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 1);
}

TEST_CASE("add-layer button is enabled right after opening the first file") {
    TileMapWidget map;
    ViewForm form(&map);
    CHECK_FALSE(form.addLayerButton()->isEnabled());
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    CHECK(form.addLayerButton()->isEnabled());
}

TEST_CASE("adding a layer with no open file is a no-op") {
    TileMapWidget map;
    ViewForm form(&map);
    form.addLayer();
    CHECK(form.layers().empty());
}

TEST_CASE("opening a WPS_GEOG dataset directory and adding a layer renders it as a raster") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geotiff_convert/wps_soiltemp_1deg"});
    CHECK(form.fileTreeWidget()->topLevelItemCount() == 1);

    form.addLayer();
    REQUIRE(form.layers().size() == 1);
    CHECK(form.layers().front().settings.variable == "Annual mean deep soil temperature");
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 1);
}

TEST_CASE("multi-selecting a series opens one file entry") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    REQUIRE(form.fileTreeWidget()->topLevelItemCount() == 1);
    auto* item = form.fileTreeWidget()->topLevelItem(0);
    CHECK(item->text(0).startsWith("wrfout_d01 (3 files"));
}

TEST_CASE("series time combo shows real timestamps") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    std::vector<std::string> labels;
    for (int i = 0; i < form.timeCombo()->count(); ++i) labels.push_back(form.timeCombo()->itemText(i).toStdString());
    CHECK(labels == std::vector<std::string>{"2020-01-01 00:00", "2020-01-01 00:30", "2020-01-01 01:00"});
}

TEST_CASE("multi-selecting a single file behaves like the normal open") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    CHECK(form.fileTreeWidget()->topLevelItemCount() == 1);
}

TEST_CASE("selecting a layer shows its properties") {
    TileMapWidget map;
    ViewForm form(&map);
    form.show();
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();
    auto* item = form.layerTreeWidget()->topLevelItem(0);
    form.layerTreeWidget()->setCurrentItem(item);
    REQUIRE(form.selectedLayerId().has_value());
    CHECK(form.selectedLayerId() == form.layers().front().layerId);
    CHECK(form.propertiesGroup()->isVisible());
}

TEST_CASE("level row hidden for a 2D variable and shown for a 3D one") {
    TileMapWidget map;
    ViewForm form(&map);
    form.show();
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    const auto twoDIndex = form.variableCombo()->findText("T2");
    REQUIRE(twoDIndex >= 0);
    form.variableCombo()->setCurrentIndex(twoDIndex);
    CHECK_FALSE(form.levelSpin()->isVisible());

    const auto threeDIndex = form.variableCombo()->findText("U");
    REQUIRE(threeDIndex >= 0);
    form.variableCombo()->setCurrentIndex(threeDIndex);
    CHECK(form.levelSpin()->isVisible());
    CHECK(form.levelLabel()->text() == "Vertical Level:");
    CHECK(form.levelSpin()->maximum() == 3);
}

TEST_CASE("time combo selection updates the layer") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.timeCombo()->setCurrentIndex(2);
    CHECK(form.layers().front().settings.timeIndex == 2);
}

TEST_CASE("unchecking layer visibility removes the overlay without removing the layer") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 1);

    auto* item = form.layerTreeWidget()->topLevelItem(0);
    item->setCheckState(0, Qt::Unchecked);
    CHECK(form.layers().size() == 1);
    CHECK_FALSE(form.layers().front().settings.visible);
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 0);

    item = form.layerTreeWidget()->topLevelItem(0);  // re-fetch: unaffected here, but good hygiene
    item->setCheckState(0, Qt::Checked);
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 1);
}

TEST_CASE("colormap and opacity changes apply to the layer") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    const auto plasmaIndex = form.colormapCombo()->findText("plasma");
    REQUIRE(plasmaIndex >= 0);
    form.colormapCombo()->setCurrentIndex(plasmaIndex);
    CHECK(form.layers().front().settings.colormap == "plasma");

    form.opacitySlider()->setValue(25);
    CHECK(form.layers().front().settings.opacity == Catch::Approx(0.25));
}

TEST_CASE("categorical colormap is auto-selected for LU_INDEX") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();  // default variable is HGT_M (continuous)
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    CHECK(form.layers().front().settings.colormap != kCategoricalColormap);

    const auto luIndex = form.variableCombo()->findText("LU_INDEX");
    REQUIRE(luIndex >= 0);
    form.variableCombo()->setCurrentIndex(luIndex);
    CHECK(form.layers().front().settings.colormap == kCategoricalColormap);
}

TEST_CASE("switching away from a categorical variable reverts the colormap") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    form.variableCombo()->setCurrentIndex(form.variableCombo()->findText("LU_INDEX"));
    CHECK(form.layers().front().settings.colormap == kCategoricalColormap);

    form.variableCombo()->setCurrentIndex(form.variableCombo()->findText("HGT_M"));
    CHECK(form.layers().front().settings.colormap != kCategoricalColormap);
}

TEST_CASE("categorical colormap can still be manually overridden") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.variableCombo()->setCurrentIndex(form.variableCombo()->findText("LU_INDEX"));
    CHECK(form.layers().front().settings.colormap == kCategoricalColormap);

    const auto plasmaIndex = form.colormapCombo()->findText("plasma");
    REQUIRE(plasmaIndex >= 0);
    form.colormapCombo()->setCurrentIndex(plasmaIndex);
    CHECK(form.layers().front().settings.colormap == "plasma");
}

TEST_CASE("units combo hidden for a variable with no known conversions") {
    TileMapWidget map;
    ViewForm form(&map);
    form.show();
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();  // HGT_M: units is blank in this fixture
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    CHECK_FALSE(form.unitsCombo()->isVisible());
}

TEST_CASE("units combo shown and changes the layer and colorbar title") {
    TileMapWidget map;
    ViewForm form(&map);
    form.show();
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.variableCombo()->setCurrentIndex(form.variableCombo()->findText("T2"));
    CHECK(form.unitsCombo()->isVisible());  // T2 is Kelvin, convertible

    const auto kelvinLegend = map.legendPixmap();
    const auto degCIndex = form.unitsCombo()->findData("degC");
    REQUIRE(degCIndex >= 0);
    form.unitsCombo()->setCurrentIndex(degCIndex);
    CHECK(form.layers().front().settings.unitKey == "degC");
    CHECK(map.legendPixmap().toImage() != kelvinLegend.toImage());
}

TEST_CASE("tick count spinbox updates the layer and the legend") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    CHECK(form.tickCountSpin()->value() == 3);  // default
    const auto before = map.legendPixmap();
    form.tickCountSpin()->setValue(7);
    CHECK(form.layers().front().settings.tickCount == 7);
    CHECK(map.legendPixmap().toImage() != before.toImage());
}

TEST_CASE("tick format combo enables the decimals spinbox only when not auto") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    CHECK_FALSE(form.tickDecimalsSpin()->isEnabled());  // "auto" by default
    const auto fixedIndex = form.tickFormatCombo()->findData("fixed");
    REQUIRE(fixedIndex >= 0);
    form.tickFormatCombo()->setCurrentIndex(fixedIndex);
    CHECK(form.layers().front().settings.tickFormat == "fixed");
    CHECK(form.tickDecimalsSpin()->isEnabled());
}

TEST_CASE("colorbar shown for the selected visible layer") {
    TileMapWidget map;
    ViewForm form(&map);
    CHECK_FALSE(map.hasLegend());  // nothing selected yet
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    CHECK(map.hasLegend());
}

TEST_CASE("colorbar hidden when the selected layer is not visible") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    CHECK(map.hasLegend());
    form.layerTreeWidget()->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
    CHECK_FALSE(map.hasLegend());
}

TEST_CASE("colorbar hidden when nothing is selected") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    CHECK(map.hasLegend());
    form.layerTreeWidget()->clearSelection();
    CHECK_FALSE(map.hasLegend());
}

TEST_CASE("colorbar follows layer selection") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();  // layer 1: HGT_M
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();  // layer 2, now selected
    const auto legendForLayerTwo = map.legendPixmap();
    REQUIRE(map.hasLegend());

    // Re-select layer 1 (bottom row, since the tree shows topmost first).
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(1));
    REQUIRE(map.hasLegend());
    CHECK(map.legendPixmap().toImage() != legendForLayerTwo.toImage());
}

TEST_CASE("manual range overrides auto") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    CHECK_FALSE(form.layers().front().settings.minimum.has_value());  // auto by default
    form.autoRangeCheck()->setChecked(false);
    form.minimumSpin()->setValue(10);
    form.maximumSpin()->setValue(200);
    form.applyFieldsToSelectedLayer();
    CHECK(form.layers().front().settings.minimum == Catch::Approx(10.0));
    CHECK(form.layers().front().settings.maximum == Catch::Approx(200.0));

    form.autoRangeCheck()->setChecked(true);
    CHECK_FALSE(form.layers().front().settings.minimum.has_value());
    CHECK_FALSE(form.layers().front().settings.maximum.has_value());
}

TEST_CASE("an invalid range throws UserError") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    form.autoRangeCheck()->setChecked(false);
    form.minimumSpin()->setValue(100);
    form.maximumSpin()->setValue(0);  // max < min
    CHECK_THROWS_AS(form.applyFieldsToSelectedLayer(), UserError);
}

TEST_CASE("removing the selected layer") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.removeSelectedLayer();
    CHECK(form.layers().empty());
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 0);
}

TEST_CASE("moving a layer up and down reorders the stack") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();  // layer 1
    form.addLayer();  // layer 2 (drawn on top)
    REQUIRE(form.layers().size() == 2);
    CHECK(form.layers()[0].layerId == 1);
    CHECK(form.layers()[1].layerId == 2);

    // Displayed tree is reversed (topmost row = topmost/last-drawn layer).
    const auto topRowId = form.layerTreeWidget()->topLevelItem(0)->data(0, Qt::UserRole).toInt();
    CHECK(topRowId == 2);

    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));  // select layer 2
    form.moveSelectedLayer(-1);  // move down
    CHECK(form.layers()[0].layerId == 2);
    CHECK(form.layers()[1].layerId == 1);

    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(1));  // re-fetch: layer 2, now bottom row
    form.moveSelectedLayer(1);  // move up
    CHECK(form.layers()[0].layerId == 1);
    CHECK(form.layers()[1].layerId == 2);
}

TEST_CASE("closing a file with no layers needs no confirmation") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.fileTreeWidget()->setCurrentItem(form.fileTreeWidget()->topLevelItem(0));
    form.closeSelectedFile();  // no layers reference it, so no confirmation dialog appears
    CHECK(form.fileTreeWidget()->topLevelItemCount() == 0);
}

TEST_CASE("two layers from different files both render") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.openFiles({"tests/fixtures/wrfout_multitime.nc"});
    form.addLayer();
    CHECK(form.layers().size() == 2);
    CHECK(map.rasterOverlayGroupSize("view-rasters") == 2);
}

TEST_CASE("zoom-to-layer moves the map") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    map.resize(400, 400);
    map.setCenter(0.0, 0.0, 2);
    form.zoomToSelectedLayer();
    // geo_em_small.nc is centered around lon ~114.17, lat ~22.3 (Hong Kong).
    CHECK(map.centerLongitude() == Catch::Approx(114.17).margin(0.1));
    CHECK(map.centerLatitude() == Catch::Approx(22.3).margin(0.1));
}

TEST_CASE("adding the first layer auto-zooms to it") {
    TileMapWidget map;
    ViewForm form(&map);
    map.resize(400, 400);
    map.setCenter(0.0, 0.0, 2);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    CHECK(map.centerLongitude() == Catch::Approx(114.17).margin(0.1));
    CHECK(map.centerLatitude() == Catch::Approx(22.3).margin(0.1));
}

TEST_CASE("adding a second layer does not recenter the map") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    map.resize(400, 400);
    map.setCenter(0.0, 0.0, 2);  // simulate the user having panned away
    form.addLayer();  // second layer, same file
    CHECK(map.centerLongitude() == Catch::Approx(0.0).margin(0.01));
    CHECK(map.centerLatitude() == Catch::Approx(0.0).margin(0.01));
}

TEST_CASE("play button disabled for a single-timestep file") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    CHECK_FALSE(form.playCheck()->isEnabled());
}

TEST_CASE("play button enabled for a multi-file series") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    CHECK(form.playCheck()->isEnabled());
}

TEST_CASE("checking play starts the timer and advances time on tick") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    form.playCheck()->setChecked(true);
    CHECK(form.playbackTimer()->isActive());
    CHECK(form.timeCombo()->currentIndex() == 0);

    form.advancePlayback();
    CHECK(form.timeCombo()->currentIndex() == 1);
}

TEST_CASE("play wraps back to the first frame after the last") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.timeCombo()->setCurrentIndex(form.timeCombo()->count() - 1);

    form.advancePlayback();
    CHECK(form.timeCombo()->currentIndex() == 0);
}

TEST_CASE("previous/next step buttons are disabled for a single-timestep file, enabled for a series") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    CHECK_FALSE(form.previousStepButton()->isEnabled());
    CHECK_FALSE(form.nextStepButton()->isEnabled());
}

TEST_CASE("next/previous step buttons move the time combo and wrap at either end") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    CHECK(form.nextStepButton()->isEnabled());
    CHECK(form.previousStepButton()->isEnabled());

    form.stepPlayback(1);
    CHECK(form.timeCombo()->currentIndex() == 1);
    form.stepPlayback(1);
    CHECK(form.timeCombo()->currentIndex() == 2);
    form.stepPlayback(1);
    CHECK(form.timeCombo()->currentIndex() == 0);  // wraps forward past the last frame

    form.stepPlayback(-1);
    CHECK(form.timeCombo()->currentIndex() == 2);  // wraps backward past the first frame
}

TEST_CASE("the play interval spin box defaults to 600ms and drives the timer, live even while playing") {
    TileMapWidget map;
    ViewForm form(&map);
    CHECK(form.playIntervalSpin()->value() == 600);
    CHECK(form.playbackTimer()->interval() == 600);

    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.playCheck()->setChecked(true);

    form.playIntervalSpin()->setValue(150);
    CHECK(form.playbackTimer()->interval() == 150);
    CHECK(form.playbackTimer()->isActive());
}

TEST_CASE("unchecking play stops the timer") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.playCheck()->setChecked(true);
    form.playCheck()->setChecked(false);
    CHECK_FALSE(form.playbackTimer()->isActive());
}

TEST_CASE("switching layer selection stops playback") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.playCheck()->setChecked(true);
    CHECK(form.playbackTimer()->isActive());

    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(1));
    CHECK_FALSE(form.playCheck()->isChecked());
    CHECK_FALSE(form.playbackTimer()->isActive());
}

TEST_CASE("info overlay hidden by default and shown when checked") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));

    CHECK_FALSE(map.hasInfoText());
    form.showInfoCheck()->setChecked(true);
    CHECK(map.hasInfoText());

    form.showInfoCheck()->setChecked(false);
    CHECK_FALSE(map.hasInfoText());
}

TEST_CASE("info overlay reports variable, date/time, and value range") {
    TileMapWidget map;
    ViewForm form(&map);
    form.openFiles({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.variableCombo()->setCurrentIndex(form.variableCombo()->findText("T2"));
    form.showInfoCheck()->setChecked(true);

    REQUIRE(map.hasInfoText());
    const auto text = map.infoText();
    CHECK(text.contains("T2"));
    CHECK(text.contains("2020-01-01 00:00"));  // the series' own first timestamp, not "Step 1 of 3"
    CHECK(text.contains("range"));
}

TEST_CASE("computed domain outlines are visually distinguishable") {
    auto project = readWpsNamelist("tests/fixtures/namelist_siblings.wps");
    const auto overlays = computeDomainOverlays(project.domains);
    REQUIRE(overlays.size() == 4);
    std::set<std::tuple<int, int, int>> colors;
    for (const auto& overlay : overlays) colors.insert({overlay.color.red(), overlay.color.green(), overlay.color.blue()});
    CHECK(colors.size() == 4);  // the 8-entry palette cycle has plenty of room for 4 domains
}

namespace {
// QWidget::event() is protected, but QApplication::sendEvent dispatches to
// it internally regardless of the caller's access - the standard way to
// drive TileMapWidget's mouse handlers (and, through them, DomainForm's
// drag hooks) from outside a QWidget subclass. Mirrors widget_tests.cpp's
// identically-named helpers.
void press(TileMapWidget& map, QPointF pos) {
    QMouseEvent event(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&map, &event);
}
void move(TileMapWidget& map, QPointF pos) {
    QMouseEvent event(QEvent::MouseMove, pos, pos, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&map, &event);
}
void release(TileMapWidget& map, QPointF pos) {
    QMouseEvent event(QEvent::MouseButtonRelease, pos, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&map, &event);
}
}  // namespace

TEST_CASE("north arrow is off by default and toggled by the View tab's checkbox") {
    TileMapWidget map;
    ViewForm form(&map);
    CHECK_FALSE(map.showNorthArrow());
    form.northArrowCheck()->setChecked(true);
    CHECK(map.showNorthArrow());
    form.northArrowCheck()->setChecked(false);
    CHECK_FALSE(map.showNorthArrow());
}

TEST_CASE("hovering the bare map widget shows lon/lat with no handler registered, and clears when the mouse leaves") {
    TileMapWidget map;
    map.resize(300, 300);
    CHECK(map.hoverText().isEmpty());
    move(map, QPointF(150, 150));
    CHECK(map.hoverText().contains("°,"));
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&map, &leave);
    CHECK(map.hoverText().isEmpty());
}

TEST_CASE("exportImage excludes the hover readout and restores it afterward") {
    TileMapWidget map;
    map.resize(300, 300);
    move(map, QPointF(150, 150));
    const auto before = map.hoverText();
    REQUIRE_FALSE(before.isEmpty());
    QTemporaryDir dir;
    const auto path = QString::fromStdString((std::filesystem::path(dir.path().toStdString()) / "export.png").string());
    CHECK(map.exportImage(path));
    CHECK(map.hoverText() == before);
}

TEST_CASE("view form's hover handler samples the topmost visible layer's value, falling back to lon/lat once it's hidden") {
    TileMapWidget map;
    ViewForm form(&map);
    map.resize(400, 400);
    form.openFiles({"tests/fixtures/geo_em_small.nc"});
    form.addLayer();
    form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
    form.zoomToSelectedLayer();

    const QPointF center(map.width() / 2.0, map.height() / 2.0);
    move(map, center);
    const auto withValue = map.hoverText();
    CHECK_FALSE(withValue.isEmpty());
    CHECK(withValue.contains("°,"));

    form.layerTreeWidget()->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
    move(map, center);
    const auto withoutValue = map.hoverText();
    CHECK(withoutValue.contains("°,"));
    // The visible layer's raster covers the map center after zoom-to-layer,
    // so hiding it should drop the sampled-value prefix, shrinking the text
    // down to just the coordinates.
    CHECK(withValue != withoutValue);
    CHECK(withValue.endsWith(withoutValue));
}

TEST_CASE("destroying the view form clears its hover handler so a later mouse-move over the map is safe") {
    TileMapWidget map;
    map.resize(300, 300);
    {
        ViewForm form(&map);
        form.openFiles({"tests/fixtures/geo_em_small.nc"});
        form.addLayer();
        form.layerTreeWidget()->setCurrentItem(form.layerTreeWidget()->topLevelItem(0));
        form.zoomToSelectedLayer();
        move(map, QPointF(150, 150));
        CHECK_FALSE(map.hoverText().isEmpty());
    }  // form destroyed here; map (declared first) outlives it, as it does for every real tab in MainWindow.
    move(map, QPointF(150, 150));
    CHECK(map.hoverText().contains("°,"));
}

TEST_CASE("dragging a root domain's outline moves its center point live") {
    TileMapWidget map;
    map.resize(500, 400);
    DomainForm form(&map);
    form.show();
    // addChild() with no domains yet creates the root: lat-lon, centered on
    // (0,0), 0.1x0.1-degree cells, 10x10 - already a complete, valid
    // domain, so rebuildTree()'s redraw(true) zooms the map to fit it and
    // the widget's exact center (500/2, 400/2) lands on lon=0/lat=0.
    form.addChild();
    REQUIRE(form.project()->domains.domains().size() == 1);
    CHECK(form.centerLonField()->text().toDouble() == Catch::Approx(0.0));
    CHECK(form.centerLatField()->text().toDouble() == Catch::Approx(0.0));

    const QPointF center(map.width() / 2.0, map.height() / 2.0);
    press(map, center);
    CHECK(map.dragTarget() == "overlay");
    move(map, center + QPointF(60, 40));
    // The panel updates live, mid-drag, before the button is even released.
    CHECK(form.centerLonField()->text().toDouble() != Catch::Approx(0.0));
    release(map, center + QPointF(60, 40));
    CHECK(map.dragTarget().isEmpty());

    const auto& root = form.project()->domains.domains().at(0);
    CHECK(root.centerLon != Catch::Approx(0.0));
    CHECK(root.centerLat != Catch::Approx(0.0));
    CHECK(form.centerLonField()->text().toDouble() == Catch::Approx(root.centerLon));
    CHECK(form.centerLatField()->text().toDouble() == Catch::Approx(root.centerLat));
}

TEST_CASE("dragging a nested domain's outline moves it within its parent without moving the parent") {
    TileMapWidget map;
    map.resize(500, 400);
    DomainForm form(&map);
    form.show();
    form.addChild();  // root
    form.domainTree()->setCurrentItem(form.domainTree()->topLevelItem(0));
    form.addChild();  // child of root

    // Widen the child so the map's zoom-to-fit centroid (which was already
    // computed from the root's own full extent - a child always sits
    // inside its parent) also lands inside the child's own ring, not just
    // the root's: ratio 1 (same cell size as the parent), 9 of the root's
    // 10 cells in each direction, no padding.
    auto* childItem = form.domainTree()->topLevelItem(0)->child(0);
    form.domainTree()->setCurrentItem(childItem);
    form.ratioField()->setText("1");
    form.columnsField()->setText("9");
    form.rowsField()->setText("9");
    form.paddingLeftField()->setText("0");
    form.paddingBottomField()->setText("0");
    REQUIRE(form.applySelectedDomainFields(true));

    const auto originalRootCenterLon = form.project()->domains.domains().at(0).centerLon;
    const auto originalRootCenterLat = form.project()->domains.domains().at(0).centerLat;

    const QPointF center(map.width() / 2.0, map.height() / 2.0);  // inside both rings; child wins (see hitTestOverlay)
    press(map, center);
    CHECK(map.dragTarget() == "overlay");
    // Padding moves in whole parent cells (0.1 degree each here), clamped
    // at 0 - dragging right/up (screen +x/-y = east/north = increasing
    // padding, away from the child's starting padding of 0) so the move
    // isn't silently absorbed by that floor. A large screen delta
    // guarantees crossing at least one cell's worth in both axes, unlike
    // the root test above (which edits a continuous field, so any nonzero
    // movement already proves it).
    const QPointF end = center + QPointF(200, -150);
    move(map, end);
    release(map, end);

    const auto& domains = form.project()->domains.domains();
    REQUIRE(domains.size() == 2);
    // The child moved (its own position fields, not the root's Center
    // Point, which is what DomainForm shows once the drag selects it)...
    CHECK(form.paddingLeftField()->text().toInt() != 0);
    CHECK(form.paddingBottomField()->text().toInt() != 0);
    CHECK(domains[1].paddingLeft == form.paddingLeftField()->text().toInt());
    CHECK(domains[1].paddingBottom == form.paddingBottomField()->text().toInt());
    // ...and the root, sitting right underneath it, did not.
    CHECK(domains[0].centerLon == Catch::Approx(originalRootCenterLon));
    CHECK(domains[0].centerLat == Catch::Approx(originalRootCenterLat));
}

TEST_CASE("dragging a root domain's corner handle resizes its grid extent") {
    TileMapWidget map;
    map.resize(400, 300);
    DomainForm form(&map);
    form.show();
    form.addChild();  // root: lat-lon, center (0,0), 0.1x0.1 cells, 10x10 -> bounds [-0.5,0.5]^2
    REQUIRE(form.project()->domains.domains().size() == 1);
    REQUIRE(form.columnsField()->text().toInt() == 10);
    REQUIRE(form.rowsField()->text().toInt() == 10);

    // Pin down a known, exact view rather than relying on zoomToBounds'
    // auto-fit, so the corner handle's screen position can be computed
    // directly from worldPixel()'s own formula (see widget_tests.cpp's
    // resize-handle tests, which use the identical formula/inputs).
    map.setCenter(0.0, 0.0, 6);
    map.grab();

    // NE handle (lon=0.5/lat=0.5) at zoom 6, centered on (0,0), 400x300.
    const QPointF neHandle(222.76, 127.24);
    press(map, neHandle);
    CHECK(map.dragTarget() == "overlay-resize");
    move(map, neHandle + QPointF(80, -60));  // drag outward: grow the domain
    // The panel updates live, mid-drag.
    CHECK(form.columnsField()->text().toInt() > 10);
    CHECK(form.rowsField()->text().toInt() > 10);
    release(map, neHandle + QPointF(80, -60));
    CHECK(map.dragTarget().isEmpty());

    const auto& root = form.project()->domains.domains().at(0);
    CHECK(root.columns == form.columnsField()->text().toInt());
    CHECK(root.rows == form.rowsField()->text().toInt());
    // The SW corner (the anchor, diagonally opposite the NE handle that
    // was dragged) stays fixed - i.e. the domain grew from its NE corner,
    // not by re-centering it symmetrically.
    CHECK(root.centerLon > 0.0);
    CHECK(root.centerLat > 0.0);
}

TEST_CASE("dragging a nested domain's corner handle resizes it without touching its parent") {
    TileMapWidget map;
    map.resize(400, 300);
    DomainForm form(&map);
    form.show();
    form.addChild();  // root: bounds [-0.5,0.5]^2 (see the root-resize test above)
    form.domainTree()->setCurrentItem(form.domainTree()->topLevelItem(0));
    form.addChild();  // child of root

    auto* childItem = form.domainTree()->topLevelItem(0)->child(0);
    form.domainTree()->setCurrentItem(childItem);
    form.ratioField()->setText("1");
    form.columnsField()->setText("9");
    form.rowsField()->setText("9");
    form.paddingLeftField()->setText("0");
    form.paddingBottomField()->setText("0");
    // Child bounds become [-0.5,-0.5] to [0.4,0.4] (root's SW corner, 9 of
    // the root's own 0.1-degree cells wide/tall).
    REQUIRE(form.applySelectedDomainFields(true));

    map.setCenter(0.0, 0.0, 6);
    map.grab();

    const int originalRootCols = form.project()->domains.domains().at(0).columns;
    const auto originalRootCenterLon = form.project()->domains.domains().at(0).centerLon;

    // The child's NE handle (lon=0.4/lat=0.4). Deliberately close to the
    // root's own NE handle (~6px away, both within the 8px hit radius) -
    // hitTestOverlayHandle checks the topmost (last-appended, i.e. the
    // child) overlay first, so this must resolve to the child regardless.
    const QPointF childNe(218.20, 131.80);
    press(map, childNe);
    CHECK(map.dragTarget() == "overlay-resize");
    move(map, childNe + QPointF(-30, 30));  // shrink it inward, away from the root's own NE
    release(map, childNe + QPointF(-30, 30));

    const auto& domains = form.project()->domains.domains();
    REQUIRE(domains.size() == 2);
    CHECK(domains[1].columns < 9);
    CHECK(domains[1].rows < 9);
    CHECK(form.columnsField()->text().toInt() == domains[1].columns);
    CHECK(form.rowsField()->text().toInt() == domains[1].rows);
    // The root (this domain's parent) was untouched.
    CHECK(domains[0].columns == originalRootCols);
    CHECK(domains[0].centerLon == Catch::Approx(originalRootCenterLon));
}

TEST_CASE("domain outlines are not draggable while another tab owns the map") {
    TileMapWidget map;
    map.resize(500, 400);
    DomainForm form(&map);
    form.show();
    form.addChild();
    form.setActive(false);  // e.g. the View tab is now the active one

    const QPointF center(map.width() / 2.0, map.height() / 2.0);
    press(map, center);
    CHECK(map.dragTarget().isEmpty());  // falls through to an ordinary map pan, not an overlay drag
    release(map, center);

    form.setActive(true);
    press(map, center);
    CHECK(map.dragTarget() == "overlay");
    release(map, center);
}

TEST_CASE("domain form selection follows real tree clicks") {
    TileMapWidget map;
    DomainForm form(&map);
    form.setProject(readWpsNamelist("tests/fixtures/namelist_siblings.wps"));
    auto* tree = form.domainTree();
    auto* domain2 = tree->topLevelItem(0)->child(0);
    tree->setCurrentItem(domain2->child(1));  // domain 4, a sibling leaf
    // setCurrentItem synchronously fires currentItemChanged, which
    // DomainForm connects to updateSelection() - no event-loop spin needed.
    CHECK(tree->currentItem()->data(0, Qt::UserRole).toInt() == 4);
}

// --- test_ui_domain_tree.py port --------------------------------------------
// DomainForm::setProject() is the entry point both the real "Import…" button
// and these tests use (like ViewForm::openFiles()).

TEST_CASE("building a sibling tree from scratch through addChild()") {
    TileMapWidget map;
    DomainForm form(&map);
    // A fresh DomainForm always has a (possibly empty) project - no
    // setProject() call here, matching the real app's startup state.
    CHECK(form.addDomainButton()->text() == "Add Root Domain");

    form.addChild();  // first click with no domains: adds the root
    CHECK(form.domainTree()->topLevelItemCount() == 1);
    CHECK(form.addDomainButton()->text() == "Add Child Domain");

    // Re-fetch the root item after every addChild() call: rebuildTree()
    // clears and rebuilds the whole QTreeWidget, so any QTreeWidgetItem*
    // from before a call is dangling afterward.
    form.domainTree()->setCurrentItem(form.domainTree()->topLevelItem(0));
    form.addChild();  // first child

    form.domainTree()->setCurrentItem(form.domainTree()->topLevelItem(0));
    form.addChild();  // second child of the same parent: a sibling

    const auto& domains = form.project()->domains.domains();
    REQUIRE(domains.size() == 3);
    CHECK(domains[1].parentId == 1);
    CHECK(domains[2].parentId == 1);

    // The tree widget itself (not just the domain list) reflects the
    // sibling shape: two children under the same parent item.
    CHECK(form.domainTree()->topLevelItem(0)->childCount() == 2);
}

TEST_CASE("importing a namelist builds the sibling tree") {
    TileMapWidget map;
    DomainForm form(&map);
    form.setProject(readWpsNamelist("tests/fixtures/namelist_siblings.wps"));

    REQUIRE(form.project().has_value());
    const auto& domains = form.project()->domains.domains();
    REQUIRE(domains.size() == 4);
    CHECK(domains[1].parentId == 1);
    CHECK(domains[2].parentId == 2);
    CHECK(domains[3].parentId == 2);

    auto* rootItem = form.domainTree()->topLevelItem(0);
    REQUIRE(rootItem->childCount() == 1);
    auto* domain2Item = rootItem->child(0);
    CHECK(domain2Item->data(0, Qt::UserRole).toInt() == 2);
    REQUIRE(domain2Item->childCount() == 2);
    std::vector<int> siblingNumbers{domain2Item->child(0)->data(0, Qt::UserRole).toInt(), domain2Item->child(1)->data(0, Qt::UserRole).toInt()};
    std::sort(siblingNumbers.begin(), siblingNumbers.end());
    CHECK(siblingNumbers == std::vector<int>{3, 4});
}

TEST_CASE("removing one sibling renumbers the survivor") {
    TileMapWidget map;
    DomainForm form(&map);
    form.setProject(readWpsNamelist("tests/fixtures/namelist_siblings.wps"));

    auto* domain2Item = form.domainTree()->topLevelItem(0)->child(0);
    auto* domain3Item = domain2Item->child(0);
    REQUIRE(domain3Item->data(0, Qt::UserRole).toInt() == 3);
    const auto survivorBoundsBefore = form.project()->domains.domains()[3].bounds;  // domain 4, untouched
    REQUIRE(survivorBoundsBefore.has_value());

    // Domain 3 is a leaf (no children of its own), so removing it doesn't
    // trigger the cascade-delete confirmation dialog.
    form.domainTree()->setCurrentItem(domain3Item);
    form.removeSelected();

    const auto& domains = form.project()->domains.domains();
    REQUIRE(domains.size() == 3);
    // Old domain 4 is renumbered to 3, still parented under (unrenumbered) domain 2.
    CHECK(domains[2].parentId == 2);
    REQUIRE(domains[2].bounds.has_value());
    CHECK(domains[2].bounds->minX == Catch::Approx(survivorBoundsBefore->minX));
    CHECK(domains[2].bounds->maxY == Catch::Approx(survivorBoundsBefore->maxY));
}

TEST_CASE("an out-of-bounds child surfaces as UserError via a field edit") {
    TileMapWidget map;
    DomainForm form(&map);
    form.setProject(readWpsNamelist("tests/fixtures/namelist_siblings.wps"));
    // Domain 2's own child (domain 3) already fits its parent by
    // construction - applying its unmodified fields must succeed first,
    // matching the Python test's own "fits by default" premise.
    auto* domain3Item = form.domainTree()->topLevelItem(0)->child(0)->child(0);
    form.domainTree()->setCurrentItem(domain3Item);
    CHECK(form.applySelectedDomainFields(/*raiseOnInvalid=*/true));

    // Push the child's position far outside the parent's extent through the
    // actual "Position within Parent" fields, same as a user typing into them.
    form.paddingLeftField()->setText("100000");
    form.paddingBottomField()->setText("100000");
    CHECK_THROWS_AS(form.applySelectedDomainFields(/*raiseOnInvalid=*/true), UserError);
}

TEST_CASE("GeotiffConvertForm defaults to the forward direction with forward-only fields enabled") {
    GeotiffConvertForm form;
    form.show();  // isVisible() needs the ancestor chain shown at least once
    CHECK(form.directionCombo()->currentIndex() == 0);
    CHECK(form.outputDirectoryField()->isVisible());
    CHECK_FALSE(form.outputTiffField()->isVisible());
    CHECK(form.borderWidthField()->isEnabled());
    CHECK(form.categoriesField()->isEnabled() == false);  // categorical starts unchecked
}

TEST_CASE("switching direction toggles the output field and disables forward-only options") {
    GeotiffConvertForm form;
    form.show();
    form.directionCombo()->setCurrentIndex(1);
    CHECK_FALSE(form.outputDirectoryField()->isVisible());
    CHECK(form.outputTiffField()->isVisible());
    CHECK_FALSE(form.borderWidthField()->isEnabled());
    CHECK_FALSE(form.categoricalCheck()->isEnabled());

    form.directionCombo()->setCurrentIndex(0);
    CHECK(form.outputDirectoryField()->isVisible());
    CHECK_FALSE(form.outputTiffField()->isVisible());
    CHECK(form.borderWidthField()->isEnabled());
}

TEST_CASE("checking categorical data enables the categories field") {
    GeotiffConvertForm form;
    CHECK_FALSE(form.categoriesField()->isEnabled());
    form.categoricalCheck()->setChecked(true);
    CHECK(form.categoriesField()->isEnabled());
    form.categoricalCheck()->setChecked(false);
    CHECK_FALSE(form.categoriesField()->isEnabled());
}

TEST_CASE("running a conversion with no input path throws UserError") {
    GeotiffConvertForm form;
    form.setOutputDirectory("/tmp");
    CHECK_THROWS_AS(form.runConversion(), UserError);
}

TEST_CASE("running a conversion with no output directory throws UserError") {
    GeotiffConvertForm form;
    form.setInputPath("tests/fixtures/geotiff_convert/utm.tif");
    CHECK_THROWS_AS(form.runConversion(), UserError);
}

TEST_CASE("an out-of-range tile size throws UserError before any worker starts") {
    GeotiffConvertForm form;
    form.setInputPath("tests/fixtures/geotiff_convert/utm.tif");
    form.setOutputDirectory("/tmp");
    form.tileSizeField()->setText("0");
    CHECK_THROWS_AS(form.runConversion(), UserError);
    CHECK_FALSE(form.isRunning());
}

TEST_CASE("checking categorical data with zero categories throws UserError") {
    GeotiffConvertForm form;
    form.setInputPath("tests/fixtures/geotiff_convert/utm.tif");
    form.setOutputDirectory("/tmp");
    form.categoricalCheck()->setChecked(true);
    form.categoriesField()->setText("0");
    CHECK_THROWS_AS(form.runConversion(), UserError);
}

TEST_CASE("a real GeoTIFF-to-geogrid conversion produces geogrid tiles and an index file") {
    GeotiffConvertForm form;
    QTemporaryDir outputDir;
    REQUIRE(outputDir.isValid());

    // Absolute paths only: runConversion() changes the process's working
    // directory to the output directory before opening the input file (see
    // GeotiffConvertForm::runConversion), so a relative input path would
    // resolve against the wrong directory once the worker thread starts.
    const auto inputPath = std::filesystem::absolute("tests/fixtures/geotiff_convert/utm.tif");
    form.setInputPath(QString::fromStdString(inputPath.string()));
    form.setOutputDirectory(outputDir.path());
    form.runConversion();
    waitWhileRunning(form);

    CHECK_FALSE(form.isRunning());
    CHECK(form.logView()->toPlainText().contains("Conversion complete."));
    CHECK(std::filesystem::exists(outputDir.filePath("index").toStdString()));
    CHECK(std::filesystem::exists(outputDir.filePath("00001-00100.00001-00100").toStdString()));
}

TEST_CASE("a geogrid round-trip via the reverse direction recreates a GeoTIFF") {
    GeotiffConvertForm forward;
    QTemporaryDir geogridDir;
    REQUIRE(geogridDir.isValid());
    const auto inputPath = std::filesystem::absolute("tests/fixtures/geotiff_convert/utm.tif");
    forward.setInputPath(QString::fromStdString(inputPath.string()));
    forward.setOutputDirectory(geogridDir.path());
    forward.runConversion();
    waitWhileRunning(forward);
    REQUIRE(std::filesystem::exists(geogridDir.filePath("index").toStdString()));

    GeotiffConvertForm reverse;
    QTemporaryDir outputDir;
    REQUIRE(outputDir.isValid());
    reverse.directionCombo()->setCurrentIndex(1);
    reverse.setInputPath(geogridDir.path());
    const auto outputTiffPath = outputDir.filePath("roundtrip.tif");
    reverse.setOutputTiffPath(outputTiffPath);
    reverse.runConversion();
    waitWhileRunning(reverse);

    CHECK_FALSE(reverse.isRunning());
    CHECK(reverse.logView()->toPlainText().contains("Conversion complete."));
    CHECK(std::filesystem::exists(outputTiffPath.toStdString()));
}

namespace {
// Reads a small single-band GeoTIFF fully into memory - used by the
// real-world fixture tests below to check actual pixel values survived a
// round trip, not just that the output file exists.
std::vector<float> readSingleBandGeoTiff(const std::string& path, int& width, int& height) {
    // convert_geotiff_lib is GDAL-free (writes via libtiff/libgeotiff
    // directly), so - unlike WrfFile - nothing earlier in the process is
    // guaranteed to have already registered GDAL's drivers; do it here so
    // this helper works even when this test case runs in isolation.
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    REQUIRE(dataset);
    width = dataset->GetRasterXSize();
    height = dataset->GetRasterYSize();
    std::vector<float> data(static_cast<std::size_t>(width) * height);
    REQUIRE(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, data.data(), width, height, GDT_Float32, 0, 0) == CE_None);
    return data;
}
}  // namespace

TEST_CASE("a real WPS_GEOG numerical tile (soiltemp_1deg) converts to a plausible-range GeoTIFF") {
    // tests/fixtures/geotiff_convert/wps_soiltemp_1deg is an unmodified
    // single geogrid tile straight from NCAR's WPS_GEOG_LOW_RES mandatory
    // download (soiltemp_1deg/index + its one tile file) - real continuous
    // (non-categorical) data, not something this project generated.
    GeotiffConvertForm reverse;
    QTemporaryDir outputDir;
    REQUIRE(outputDir.isValid());
    reverse.directionCombo()->setCurrentIndex(1);
    reverse.setInputPath(QString::fromStdString(std::filesystem::absolute("tests/fixtures/geotiff_convert/wps_soiltemp_1deg").string()));
    const auto outputTiffPath = outputDir.filePath("soiltemp.tif");
    reverse.setOutputTiffPath(outputTiffPath);
    reverse.runConversion();
    waitWhileRunning(reverse);

    CHECK_FALSE(reverse.isRunning());
    CHECK(reverse.logView()->toPlainText().contains("Conversion complete."));

    int width = 0, height = 0;
    const auto data = readSingleBandGeoTiff(outputTiffPath.toStdString(), width, height);
    CHECK(width == 180);
    CHECK(height == 180);
    // Annual mean deep soil temperature in Kelvin, over a 1-degree global
    // grid where ocean pixels carry the index's missing_value=0.0 - every
    // non-missing decoded value (already scaled by scale_factor=0.01)
    // should land well within a physically sane range, not raw unscaled
    // integers or noise, and at least some land pixels must be present.
    int landPixels = 0;
    for (const auto value : data) {
        if (value == 0.0f) continue;
        CHECK(value > 200.0f);
        CHECK(value < 320.0f);
        ++landPixels;
    }
    CHECK(landPixels > 0);
}

TEST_CASE("a real WPS_GEOG numerical tile round-trips forward again into a new geogrid tile") {
    GeotiffConvertForm reverse;
    QTemporaryDir geotiffDir;
    REQUIRE(geotiffDir.isValid());
    reverse.directionCombo()->setCurrentIndex(1);
    reverse.setInputPath(QString::fromStdString(std::filesystem::absolute("tests/fixtures/geotiff_convert/wps_soiltemp_1deg").string()));
    const auto tiffPath = geotiffDir.filePath("soiltemp.tif");
    reverse.setOutputTiffPath(tiffPath);
    reverse.runConversion();
    waitWhileRunning(reverse);
    REQUIRE(std::filesystem::exists(tiffPath.toStdString()));

    GeotiffConvertForm forward;
    QTemporaryDir geogridDir;
    REQUIRE(geogridDir.isValid());
    forward.setInputPath(tiffPath);
    forward.setOutputDirectory(geogridDir.path());
    forward.runConversion();
    waitWhileRunning(forward);

    CHECK_FALSE(forward.isRunning());
    CHECK(forward.logView()->toPlainText().contains("Conversion complete."));
    CHECK(std::filesystem::exists(geogridDir.filePath("index").toStdString()));
    // The 180x180 GeoTIFF gets split into 100-pixel tiles (the form's
    // default tile size), so the first tile file covers only 1-100.
    CHECK(std::filesystem::exists(geogridDir.filePath("00001-00100.00001-00100").toStdString()));

    std::ifstream indexFile(geogridDir.filePath("index").toStdString());
    std::ostringstream indexContent;
    indexContent << indexFile.rdbuf();
    CHECK(indexContent.str().find("categorical") == std::string::npos);  // stays continuous, not miscategorized
}

TEST_CASE("a real-derived categorical GeoTIFF forward-converts to a categorical geogrid index") {
    // tests/fixtures/geotiff_convert/landuse_dominant_category.tif is a
    // small 24x24 crop, derived (dominant-category-per-pixel argmax) from a
    // real NCAR WPS_GEOG_LOW_RES modis_landuse_20class_5m_with_lakes tile -
    // genuine per-pixel MODIFIED_IGBP_MODIS_NOAH category codes (1-21), not
    // synthetic data. There is no single-layer type=categorical dataset in
    // NCAR's own mandatory download to pull directly (its landuse/soiltype
    // sets ship as per-category continuous fraction layers instead), so this
    // is the closest real-world stand-in for exercising this tool's own
    // categorical option end to end.
    GeotiffConvertForm forward;
    QTemporaryDir geogridDir;
    REQUIRE(geogridDir.isValid());
    const auto inputPath = std::filesystem::absolute("tests/fixtures/geotiff_convert/landuse_dominant_category.tif");
    forward.setInputPath(QString::fromStdString(inputPath.string()));
    forward.setOutputDirectory(geogridDir.path());
    forward.categoricalCheck()->setChecked(true);
    forward.categoriesField()->setText("21");
    forward.runConversion();
    waitWhileRunning(forward);

    CHECK_FALSE(forward.isRunning());
    CHECK(forward.logView()->toPlainText().contains("Conversion complete."));
    REQUIRE(std::filesystem::exists(geogridDir.filePath("index").toStdString()));

    std::ifstream indexFile(geogridDir.filePath("index").toStdString());
    std::ostringstream indexContent;
    indexContent << indexFile.rdbuf();
    const auto index = indexContent.str();
    CHECK(index.find("type = categorical") != std::string::npos);
    CHECK(index.find("category_min = 1") != std::string::npos);
    CHECK(index.find("category_max = 22") != std::string::npos);  // categorical_range (21) + 1, per convert.cpp
}

TEST_CASE("that categorical geogrid round-trips back to a GeoTIFF with the same category values") {
    GeotiffConvertForm forward;
    QTemporaryDir geogridDir;
    REQUIRE(geogridDir.isValid());
    const auto inputPath = std::filesystem::absolute("tests/fixtures/geotiff_convert/landuse_dominant_category.tif");
    forward.setInputPath(QString::fromStdString(inputPath.string()));
    forward.setOutputDirectory(geogridDir.path());
    forward.categoricalCheck()->setChecked(true);
    forward.categoriesField()->setText("21");
    // Match the tile size to the fixture's own 24x24 extent - the geogrid
    // reader infers the overall raster size purely from the union of tile
    // filenames on disk (there's no separate stored width/height), so the
    // default 100px tile size would pad the round trip out to 100x100.
    forward.tileSizeField()->setText("24");
    forward.runConversion();
    waitWhileRunning(forward);
    REQUIRE(std::filesystem::exists(geogridDir.filePath("index").toStdString()));

    GeotiffConvertForm reverse;
    QTemporaryDir outputDir;
    REQUIRE(outputDir.isValid());
    reverse.directionCombo()->setCurrentIndex(1);
    reverse.setInputPath(geogridDir.path());
    const auto outputTiffPath = outputDir.filePath("landuse_roundtrip.tif");
    reverse.setOutputTiffPath(outputTiffPath);
    reverse.runConversion();
    waitWhileRunning(reverse);
    CHECK(reverse.logView()->toPlainText().contains("Conversion complete."));

    int originalWidth = 0, originalHeight = 0;
    const auto original = readSingleBandGeoTiff(inputPath.string(), originalWidth, originalHeight);
    int roundTripWidth = 0, roundTripHeight = 0;
    const auto roundTrip = readSingleBandGeoTiff(outputTiffPath.toStdString(), roundTripWidth, roundTripHeight);

    REQUIRE(roundTripWidth == originalWidth);
    REQUIRE(roundTripHeight == originalHeight);
    // Category codes are small integers written with no lossy predictor, so
    // the round trip must reproduce them exactly - except right at this
    // fixture's own domain edge. tileSizeField() above was set to exactly
    // match the fixture's extent (one tile = the whole "domain"), which
    // real WPS_GEOG data never does; every real dataset's tiles keep this
    // few-pixel edge margin filled by a *neighboring* tile's real data. A
    // margin of tile_bdr (3) pixels around a truly edgeless single-tile
    // domain has nothing to source from, so it's excluded here rather than
    // asserted on.
    constexpr int margin = 3;
    for (int y = margin; y < originalHeight - margin; ++y) {
        for (int x = margin; x < originalWidth - margin; ++x) {
            const auto i = static_cast<std::size_t>(y) * originalWidth + x;
            CHECK(roundTrip[i] == original[i]);
        }
    }
}

// --- Options > Theme menu ---------------------------------------------

TEST_CASE("Options > Theme defaults to System and switches to Light/Dark live") {
    // MainWindow's theme menu persists via a default-constructed
    // QSettings() (see main.cpp/test main() above) - clear it first so an
    // earlier test case (or a previous run) can't leave this one starting
    // from an unexpected preference.
    { QSettings settings; settings.clear(); }

    MainWindow window;
    REQUIRE(window.systemThemeAction());
    REQUIRE(window.lightThemeAction());
    REQUIRE(window.darkThemeAction());
    CHECK(window.systemThemeAction()->isChecked());
    CHECK_FALSE(window.lightThemeAction()->isChecked());
    CHECK_FALSE(window.darkThemeAction()->isChecked());

    window.darkThemeAction()->trigger();
    CHECK(window.darkThemeAction()->isChecked());
    CHECK_FALSE(window.systemThemeAction()->isChecked());  // QActionGroup exclusivity
    CHECK(qApp->palette().color(QPalette::Window) == darkPalette().color(QPalette::Window));
    {
        QSettings settings;
        CHECK(themePreference(settings) == ThemePreference::Dark);
    }

    window.lightThemeAction()->trigger();
    CHECK(window.lightThemeAction()->isChecked());
    CHECK_FALSE(window.darkThemeAction()->isChecked());
    CHECK(qApp->palette().color(QPalette::Window) != darkPalette().color(QPalette::Window));
    {
        QSettings settings;
        CHECK(themePreference(settings) == ThemePreference::Light);
    }
}

TEST_CASE("a persisted Dark preference is restored the next time MainWindow is constructed") {
    {
        QSettings settings;
        settings.clear();
        setThemePreference(settings, ThemePreference::Dark);
    }

    MainWindow window;
    CHECK(window.darkThemeAction()->isChecked());
    CHECK(qApp->palette().color(QPalette::Window) == darkPalette().color(QPalette::Window));
}
