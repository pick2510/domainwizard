#include "wrftools/domain.hpp"
#include "wrftools/error.hpp"
#include "wrftools/wrf_series.hpp"
#include "wrftools/wrf_file.hpp"
#include "wrftools/wps_namelist.hpp"
#include "wrftools/colormaps.hpp"
#include "wrftools/units.hpp"
#include "wrftools/raster_layer.hpp"
#include "wrftools/warp.hpp"
#include "wrftools/layer_renderer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <limits>

using namespace wrftools;

TEST_CASE("WRF series names are parsed and ordered") {
    const auto parsed = parseWrfFilename("wrfout_d03_2025-03-14_00_30_00");
    REQUIRE(parsed);
    CHECK(parsed->kind == "wrfout");
    CHECK(parsed->domain == "03");
    const auto grouped = groupWrfPaths({"wrfout_d03_2025-03-14_01_00_00", "wrfout_d03_2025-03-14_00_30_00", "geo_em.d01.nc"});
    REQUIRE(grouped.groups.size() == 1);
    CHECK(grouped.groups[0][0].filename() == "wrfout_d03_2025-03-14_00_30_00");
    CHECK(grouped.singles.size() == 1);
}

TEST_CASE("WRF naming accepts every supported separator and rejects invalid names") {
    const auto colon = parseWrfFilename("wrfout_d03_2025-03-14_00:30:00");
    REQUIRE(colon);
    const auto met = parseWrfFilename("met_em.d01.2025-03-14_00:00:00.nc");
    REQUIRE(met);
    CHECK(met->kind == "met_em");
    CHECK_FALSE(parseWrfFilename("geo_em.d01.nc"));
    CHECK_FALSE(parseWrfFilename("wrfinput_d01"));
    CHECK_FALSE(parseWrfFilename("wrfout_d03_2025-02-30_00_00_00"));
    CHECK_FALSE(parseWrfFilename("wrfout_d03_2025-03-14_25_00_00"));
}

TEST_CASE("WRF grouping keeps domains separate and lone files single") {
    const auto grouped = groupWrfPaths({"wrfout_d01_2020-01-01_00_00_00", "wrfout_d02_2020-01-01_00_00_00", "wrfout_d01_2020-01-01_00_30_00", "geo_em.d01.nc"});
    REQUIRE(grouped.groups.size() == 1);
    CHECK(grouped.groups.front().size() == 2);
    REQUIRE(grouped.singles.size() == 2);
    CHECK(std::find(grouped.singles.begin(), grouped.singles.end(), "wrfout_d02_2020-01-01_00_00_00") != grouped.singles.end());
}

TEST_CASE("WRF series opens lazily and reads a selected timestamp") {
    WrfFileSeries series({"tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    CHECK(series.openedFileCount() == 1);
    REQUIRE(series.times().size() == 3);
    CHECK(series.times()[1] == "2020-01-01 00:30");
    CHECK(series.read("T2", 1).size() == 64);
    CHECK(series.openedFileCount() == 2);
    CHECK_THROWS_AS(series.read("T2", 3), UserError);
}

TEST_CASE("domain projects accept siblings and reject invalid parent ids") {
    DomainProject project({
        Domain{.id = 1, .parentId = 1, .ratio = 1, .columns = 100, .rows = 100},
        Domain{.id = 2, .parentId = 1, .ratio = 3, .columns = 30, .rows = 30},
        Domain{.id = 3, .parentId = 1, .ratio = 3, .columns = 30, .rows = 30},
    });
    CHECK(project.domains().size() == 3);
    CHECK_THROWS_AS(DomainProject({Domain{.id = 1, .parentId = 1, .ratio = 1, .columns = 10, .rows = 10}, Domain{.id = 2, .parentId = 2, .ratio = 3, .columns = 3, .rows = 3}}), UserError);
}

TEST_CASE("domain containment and subtree removal are validated") {
    Domain parent{.id = 1, .parentId = 1, .columns = 100, .rows = 100, .bounds = Bounds{0, 0, 100, 100}};
    Domain child{.id = 2, .parentId = 1, .ratio = 3, .columns = 10, .rows = 10, .bounds = Bounds{10, 10, 20, 20}};
    Domain sibling{.id = 3, .parentId = 1, .ratio = 3, .columns = 10, .rows = 10, .bounds = Bounds{30, 30, 40, 40}};
    Domain grandchild{.id = 4, .parentId = 2, .ratio = 3, .columns = 5, .rows = 5, .bounds = Bounds{12, 12, 15, 15}};
    DomainProject project({parent, child, sibling, grandchild});
    project.removeSubtree(2);
    REQUIRE(project.domains().size() == 2);
    CHECK(project.domains()[1].id == 2);
    CHECK(project.domains()[1].parentId == 1);
    child.bounds = Bounds{90, 90, 110, 110};
    CHECK_THROWS_AS(DomainProject({parent, child}), UserError);
}

TEST_CASE("GDAL-backed reader discovers fixture variables") {
    WrfFile file("tests/fixtures/geo_em_small.nc");
    REQUIRE_FALSE(file.variables().empty());
    CHECK(std::any_of(file.variables().begin(), file.variables().end(), [](const WrfVariable& value) { return value.name == "LU_INDEX"; }));
    CHECK(file.geographicBounds().north > file.geographicBounds().south);
    CHECK(file.geographicBounds().east > file.geographicBounds().west);
    CHECK_FALSE(file.projectionWkt().empty());
}

// Pinned against the Python reference (wrftools.wrfreader.WRFFile) run
// against the same fixtures: `uv run python -c "from wrftools.wrfreader
// import WRFFile; f = WRFFile(path); print(f.geotransform)"`. Both fixtures
// are Mercator (MAP_PROJ=3) - exactly the projection the old
// srs.SetMercator(truelat1, standLon, 1.0, ...) construction got wrong
// (truelat1 belongs at lat_ts, not as a scale-1 origin latitude).
TEST_CASE("geotransform matches the Python reference for Mercator fixtures") {
    for (const char* path : {"tests/fixtures/geo_em_small.nc", "tests/fixtures/wrfout_multitime.nc"}) {
        WrfFile file(path);
        const auto gt = file.geotransform();
        CHECK(gt[0] == Catch::Approx(-3118.452108972693).epsilon(1e-6));
        CHECK(gt[1] == Catch::Approx(250.05003010343663).epsilon(1e-6));
        CHECK(gt[2] == 0.0);
        CHECK(gt[3] == Catch::Approx(2353541.4751173565).epsilon(1e-6));
        CHECK(gt[4] == 0.0);
        CHECK(gt[5] == Catch::Approx(-249.9877820990514).epsilon(1e-6));
        CHECK(file.projectionWkt().find("Mercator") != std::string::npos);
    }
}

TEST_CASE("GDAL-backed reader reads real multi-time rasters") {
    WrfFile file("tests/fixtures/wrfout_multitime.nc");
    REQUIRE(std::any_of(file.variables().begin(), file.variables().end(), [](const WrfVariable& value) { return value.name == "T2"; }));
    const auto first = file.read("T2", 1);
    const auto second = file.read("T2", 2);
    REQUIRE(first.size() == 64);
    REQUIRE(second.size() == 64);
    const auto firstMean = std::accumulate(first.begin(), first.end(), 0.0) / static_cast<double>(first.size());
    const auto secondMean = std::accumulate(second.begin(), second.end(), 0.0) / static_cast<double>(second.size());
    CHECK(secondMean > firstMean);
    CHECK_THROWS_AS(file.read("T2", 99), UserError);
    CHECK_THROWS_AS(file.read("NOT_A_REAL_VARIABLE"), UserError);
}

TEST_CASE("GDAL-backed reader reports dimensions and destaggers wind fields") {
    WrfFile file("tests/fixtures/wrfout_multitime.nc");
    CHECK(file.size() == std::array<int, 2>{8, 8});
    const auto wind = std::find_if(file.variables().begin(), file.variables().end(), [](const WrfVariable& value) { return value.name == "U"; });
    REQUIRE(wind != file.variables().end());
    CHECK(wind->timeCount == 3);
    CHECK(wind->levelCount == 3);
    const auto values = file.read("U", 0, 0);
    CHECK(values.size() == 64);
    CHECK_THROWS_AS(file.read("U", 0, 3), UserError);
}

TEST_CASE("WPS sibling fixture imports and exports") {
    const auto project = readWpsNamelist("tests/fixtures/namelist_siblings.wps");
    REQUIRE(project.domains.domains().size() == 4);
    CHECK(project.domains.domains()[2].parentId == 2);
    CHECK(project.domains.domains()[3].parentId == 2);
    const auto output = std::filesystem::temp_directory_path() / "wrftools-cpp-namelist.wps";
    writeWpsNamelist(project, output);
    CHECK(readWpsNamelist(output).domains.domains().size() == 4);
    std::filesystem::remove(output);
}

// Pinned against gis4wrf.core.Project.bboxes for the same fixture:
// `uv run python -c "from gis4wrf.core.readers.namelist import
// read_namelist; from gis4wrf.core.transforms.wps_namelist_to_project
// import convert_wps_nml_to_project; from gis4wrf.core.project import
// Project; nml = read_namelist(path, schema_name='wps'); project =
// convert_wps_nml_to_project(nml, Project.create()); print([bbox for bbox
// in project.bboxes])"` - domains 3/4 are Lambert siblings sharing domain
// 2 as their parent, exactly the tree shape fillDomains's single
// forward pass has to get right.
TEST_CASE("fillDomains bboxes match the Python reference for a sibling tree") {
    auto project = readWpsNamelist("tests/fixtures/namelist_siblings.wps");
    project.domains.fillDomains();
    const auto& domains = project.domains.domains();
    const std::array<Bounds, 4> expected{{
        {-281249.99999999994, -284375.0000000014, 281250.00000000006, 284374.9999999986},
        {-156249.99999999994, -159375.0000000014, 156250.00000000006, 159374.9999999986},
        {15000.000000000058, 8124.999999998603, 65000.00000000006, 59374.9999999986},
        {-61249.99999999994, -64375.0000000014, -11249.999999999942, -13125.000000001397},
    }};
    for (std::size_t i = 0; i < domains.size(); ++i) {
        REQUIRE(domains[i].bounds.has_value());
        CHECK(domains[i].bounds->minX == Catch::Approx(expected[i].minX).margin(1e-3));
        CHECK(domains[i].bounds->minY == Catch::Approx(expected[i].minY).margin(1e-3));
        CHECK(domains[i].bounds->maxX == Catch::Approx(expected[i].maxX).margin(1e-3));
        CHECK(domains[i].bounds->maxY == Catch::Approx(expected[i].maxY).margin(1e-3));
    }
}

TEST_CASE("WPS export preserves nesting and root projection fields") {
    const auto original = readWpsNamelist("tests/fixtures/namelist_siblings.wps");
    const auto output = std::filesystem::temp_directory_path() / "wrftools-cpp-roundtrip.wps";
    writeWpsNamelist(original, output);
    const auto reread = readWpsNamelist(output);
    CHECK(reread.domains.domains().front().mapProj == original.domains.domains().front().mapProj);
    CHECK(std::abs(reread.domains.domains().front().centerLon - original.domains.domains().front().centerLon) < 1e-12);
    CHECK(reread.domains.domains()[1].ratio == original.domains.domains()[1].ratio);
    CHECK(reread.domains.domains()[3].paddingBottom == original.domains.domains()[3].paddingBottom);
    std::filesystem::remove(output);
}

TEST_CASE("WPS parser gives user errors for malformed domain cardinality") {
    const auto path = std::filesystem::temp_directory_path() / "wrftools-cpp-invalid.wps";
    std::ofstream out(path);
    out << "&share\n max_dom = 2,\n/\n&geogrid\n parent_id = 1,\n parent_grid_ratio = 1,\n i_parent_start = 1,\n j_parent_start = 1,\n e_we = 10,\n e_sn = 10,\n map_proj = 'mercator',\n dx = 1000,\n dy = 1000,\n ref_lon = 0,\n ref_lat = 0,\n/\n";
    out.close();
    CHECK_THROWS_AS(readWpsNamelist(path), UserError);
    std::filesystem::remove(path);
}

TEST_CASE("WPS parser handles array values wrapped across lines") {
    // tests/fixtures/namelist_wrapped.wps deliberately wraps parent_id and
    // j_parent_start mid-array onto a continuation line with no '=' on it -
    // valid Fortran namelist syntax the old line-oriented-only parser
    // silently truncated (it would see "parent_id = 1," as a complete
    // one-element array and drop the wrapped "1," entirely).
    const auto project = readWpsNamelist("tests/fixtures/namelist_wrapped.wps");
    const auto& domains = project.domains.domains();
    REQUIRE(domains.size() == 2);
    CHECK(domains[0].parentId == 1);
    CHECK(domains[1].parentId == 1);
    CHECK(domains[1].paddingLeft == 9);   // i_parent_start = 10 -> 0-based 9
    CHECK(domains[1].paddingBottom == 9); // j_parent_start wrapped across two lines
    CHECK(domains[1].columns == 60);      // e_we = 61 -> domain_size 60
}

TEST_CASE("WPS linear-chain fixture remains valid") {
    const auto project = readWpsNamelist("tests/fixtures/namelist_hongkong.wps");
    REQUIRE(project.domains.domains().size() == 3);
    CHECK(project.domains.domains()[1].parentId == 1);
    CHECK(project.domains.domains()[2].parentId == 2);
}

TEST_CASE("unit conversions match WRF display choices") {
    const auto temperature = conversionsFor(" K ");
    REQUIRE(temperature.size() == 3);
    CHECK(convert(273.15, findUnit("K", "degC")) == Catch::Approx(0.0));
    std::vector<float> wind{1.0f, 2.0f};
    convertInPlace(wind, findUnit("m/s", "kmh"));
    CHECK(wind == std::vector<float>{3.6f, 7.2f});
    CHECK_THROWS_AS(findUnit("K", "kn"), std::out_of_range);
}

TEST_CASE("colormaps preserve end points and transparent no-data") {
    const auto& lut = colormap("viridis");
    CHECK(lut.front() == Rgb{68, 1, 84});
    CHECK(lut.back() == Rgb{253, 231, 37});
    const std::vector<float> values{0.0f, 0.5f, 1.0f, std::numeric_limits<float>::quiet_NaN()};
    const auto image = applyColormap(values, 0, 1, lut);
    CHECK(image.front() == Rgba{68, 1, 84, 255});
    CHECK(image[2] == Rgba{253, 231, 37, 255});
    CHECK(image.back()[3] == 0);
}

TEST_CASE("categorical colormap indexes classes directly") {
    const std::vector<float> values{1.0f, 2.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()};
    const auto legend = categoricalLut("USGS", 1, 2);
    const auto pixels = applyCategoricalColormap(values, legend.lut);
    CHECK(pixels[0][3] == 255);
    CHECK(pixels[1] != pixels[0]);
    CHECK(pixels[2][3] == 0);
    CHECK(pixels[3][3] == 0);
}

// Pinned against gis4wrf.core.readers.categories.LANDUSE: `uv run python -c
// "from gis4wrf.core.readers.categories import LANDUSE;
// print(LANDUSE['USGS'][1])"` -> ('Urban and Built-Up Land', '#FF0000').
TEST_CASE("categorical LANDUSE table matches the Python reference for known values") {
    const auto usgs = categoricalLut("USGS", 1, 3);
    CHECK(usgs.labels.at(1) == "Urban and Built-Up Land");
    CHECK(usgs.lut[1] == Rgb{0xFF, 0x00, 0x00});
    CHECK(usgs.labels.at(2) == "Dryland Cropland and Pasture");

    const auto modis = categoricalLut("MODIFIED_IGBP_MODIS_NOAH", 1, 1);
    CHECK(modis.labels.at(1) == "Evergreen Needleleaf Forest");
    CHECK(modis.lut[1] == Rgb{0x00, 0x80, 0x00});

    // An unknown scheme (e.g. a soil-type field, which has no table) falls
    // back to a generated label/color rather than failing.
    const auto soil = categoricalLut("", 5, 5);
    CHECK(soil.labels.at(5) == "Category 5");
}

// Pinned against `wrftools.rasterlayer._warp_to_web_mercator` run on the
// same fixture/variable/time: `uv run python -c "from wrftools.rasterlayer
// import _warp_to_web_mercator; from wrftools.wrfreader import WRFFile; f =
// WRFFile(path); print(_warp_to_web_mercator(f.read('T2', 1, 0), f.crs.wkt,
// f.geotransform))"`.
TEST_CASE("warped raster bounds match the Python reference") {
    WrfFile file("tests/fixtures/wrfout_multitime.nc");
    const auto values = file.read("T2", 1, 0);
    const auto size = file.size();
    const auto warped = warpToWebMercator(values, size[0], size[1], file.projectionWkt(), file.geotransform());
    CHECK(warped.width == 8);
    CHECK(warped.height == 8);
    CHECK(warped.bounds3857.minX == Catch::Approx(12706639.339934358).epsilon(1e-6));
    CHECK(warped.bounds3857.minY == Catch::Approx(2544877.2370938584).epsilon(1e-6));
    CHECK(warped.bounds3857.maxX == Catch::Approx(12708803.936988916).epsilon(1e-6));
    CHECK(warped.bounds3857.maxY == Catch::Approx(2547041.834148416).epsilon(1e-6));
}

TEST_CASE("raster layers render native WRF data with auto and manual ranges") {
    WrfSourceRegistry registry;
    auto& source = registry.open({"tests/fixtures/wrfout_multitime.nc"});
    const auto automatic = renderLayer(source, {.variable = "T2"});
    REQUIRE(automatic.pixels.size() == 64);
    CHECK(automatic.width == 8);
    CHECK(automatic.maximum > automatic.minimum);
    CHECK(automatic.bounds3857.maxX > automatic.bounds3857.minX);
    CHECK(automatic.bounds3857.maxY > automatic.bounds3857.minY);
    const auto manual = renderLayer(source, {.variable = "T2", .minimum = 270.0f, .maximum = 310.0f, .unitKey = "native"});
    CHECK(manual.minimum == 270.0f);
    CHECK(manual.maximum == 310.0f);
    CHECK(manual.pixels != automatic.pixels);
}

TEST_CASE("LayerRenderer caches slices/images and matches uncached rendering") {
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    LayerRenderer renderer;
    static_cast<void>(renderer.openFile({path}));
    const RasterLayer layer{.variable = "T2", .timeIndex = 1};
    const auto cached = renderer.render(path, layer);

    WrfSourceRegistry registry;
    auto& source = registry.open({path});
    const auto uncached = renderLayer(source, layer);
    CHECK(cached.pixels == uncached.pixels);
    CHECK(cached.minimum == uncached.minimum);
    CHECK(cached.bounds3857.minX == uncached.bounds3857.minX);

    // A second render() with identical settings is an image-cache hit -
    // same result, not a rebuild.
    const auto again = renderer.render(path, layer);
    CHECK(again.pixels == cached.pixels);

    // Different display units on the SAME (file, variable, time, level)
    // share one cached warped slice - converting for one layer must not
    // corrupt what a second layer in a different unit reads from it.
    const auto celsius = renderer.render(path, RasterLayer{.variable = "T2", .timeIndex = 1, .unitKey = "degC"});
    const auto kelvin = renderer.render(path, RasterLayer{.variable = "T2", .timeIndex = 1, .unitKey = "native"});
    CHECK(celsius.minimum != kelvin.minimum);  // different units, not accidentally sharing converted state
    const auto kelvinAgain = renderer.render(path, RasterLayer{.variable = "T2", .timeIndex = 1, .unitKey = "native"});
    CHECK(kelvinAgain.minimum == kelvin.minimum);  // native reading still intact after the degC render

    // invalidateFile drops the cache; the file can still be reopened and
    // rendered afterward (not left in a broken state).
    renderer.invalidateFile(path);
    CHECK(renderer.openPaths().empty());
    static_cast<void>(renderer.openFile({path}));
    const auto afterInvalidate = renderer.render(path, layer);
    CHECK(afterInvalidate.pixels == uncached.pixels);
}
