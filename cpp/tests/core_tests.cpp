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
#include "wrftools/colorbar.hpp"
#include "wrftools/wps_binary_source.hpp"
#include "wrftools/wrf_source.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <limits>
#include <memory>
#include <set>

#include <gdal_priv.h>

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

TEST_CASE("grouping edge cases: lone recognized file and input-order independence") {
    const auto lone = groupWrfPaths({"tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc"});
    CHECK(lone.groups.empty());
    REQUIRE(lone.singles.size() == 1);

    const std::vector<std::filesystem::path> ordered{
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"};
    auto reversed = ordered;
    std::reverse(reversed.begin(), reversed.end());
    const auto grouped = groupWrfPaths(reversed);
    REQUIRE(grouped.groups.size() == 1);
    CHECK(grouped.groups.front() == ordered);
}

TEST_CASE("WRF series read matches reading the underlying file directly") {
    const std::vector<std::filesystem::path> paths{
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"};
    WrfFileSeries series(paths);
    WrfFile direct("tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc");
    CHECK(series.read("T2", 1) == direct.read("T2", 0, 0));
}

TEST_CASE("WRF series name and path describe the group") {
    const std::vector<std::filesystem::path> paths{
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"};
    WrfFileSeries series(paths);
    CHECK(series.name() == "wrfout_d01 (3 files, 2020-01-01 00:00 - 2020-01-01 01:00)");
    CHECK(series.path() == paths.front());
}

TEST_CASE("WRF series rereading the same file does not reopen it") {
    WrfFileSeries series({"tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"});
    CHECK(series.openedFileCount() == 1);
    CHECK_NOTHROW(series.read("T2", 0));
    CHECK(series.openedFileCount() == 1);
    CHECK(series.isFileOpen(1) == false);
}

TEST_CASE("WRF series rejects a mismatched grid at construction when it must open eagerly") {
    // wrfout_with_1d_var.nc's name doesn't match the series pattern, so
    // pairing it with a real series member forces the eager-open fallback,
    // which validates every file's grid up front.
    CHECK_THROWS_AS(WrfFileSeries({"tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_with_1d_var.nc"}), UserError);
}

TEST_CASE("WRF series grid mismatch several files in is only caught when read") {
    // wrfout_d01_2020-01-01_02_00_00.nc parses as a real series member (so
    // construction takes the fast lazy path and does not open it) but
    // actually shares wrfout_with_1d_var.nc's different grid - the
    // mismatch only surfaces once that file is actually read.
    WrfFileSeries series({
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_02_00_00.nc"});
    CHECK(series.openedFileCount() == 1);
    CHECK_NOTHROW(series.read("T2", 0));
    CHECK_NOTHROW(series.read("T2", 2));
    CHECK_THROWS_AS(series.read("T2", 3), UserError);
}

TEST_CASE("WRF series with a multi-timestep file falls back to eager open") {
    // wrfout_multitime.nc's own filename doesn't match the series pattern
    // at all, so pairing it with a real series member exercises the "can't
    // trust the filename for timestep count" fallback path.
    WrfFileSeries series({"tests/fixtures/wrfout_multitime.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc"});
    CHECK(series.openedFileCount() == 2);
    CHECK(series.isFileOpen(0));
    CHECK(series.isFileOpen(1));
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

// Ported from tests/test_wrfreader.py's remaining cases not already
// covered above.
TEST_CASE("geo_em fixture exposes exactly its two static 2D variables") {
    WrfFile file("tests/fixtures/geo_em_small.nc");
    std::set<std::string> names;
    for (const auto& variable : file.variables()) names.insert(variable.name);
    CHECK(names == std::set<std::string>{"HGT_M", "LU_INDEX"});
    for (const auto& variable : file.variables()) {
        CHECK_FALSE(variable.extraDimension.has_value());
        CHECK(variable.timeCount == 1);
        CHECK(variable.levelCount == 1);
    }
}

// Regression coverage for a real bug: GDAL exposes 1D vertical-only
// variables (ZS/DZS, MemoryOrder "Z  ") as tiny (N,1) "rasters" that
// corrupted mass-grid-size inference and silently dropped every real 2D
// variable from a genuine wrfout file - see wrf_file.cpp's MemoryOrder
// filter.
TEST_CASE("1D vertical-only variables are excluded and don't break 2D ones") {
    WrfFile file("tests/fixtures/wrfout_with_1d_var.nc");
    const auto& variables = file.variables();
    CHECK_FALSE(std::any_of(variables.begin(), variables.end(), [](const WrfVariable& v) { return v.name == "ZS"; }));
    CHECK_FALSE(std::any_of(variables.begin(), variables.end(), [](const WrfVariable& v) { return v.name == "DZS"; }));
    CHECK(std::any_of(variables.begin(), variables.end(), [](const WrfVariable& v) { return v.name == "T2"; }));
    CHECK(std::any_of(variables.begin(), variables.end(), [](const WrfVariable& v) { return v.name == "HGT"; }));
    CHECK(file.size() == std::array<int, 2>{8, 8});
}

TEST_CASE("WrfFile rejects an unsupported MAP_PROJ") {
    CHECK_THROWS_AS(WrfFile("tests/fixtures/wrf_unknown_projection.nc"), UnsupportedError);
}

TEST_CASE("WrfFile rejects a rotated-pole lat/lon projection") {
    CHECK_THROWS_AS(WrfFile("tests/fixtures/wrf_rotated_pole.nc"), UnsupportedError);
}

TEST_CASE("WrfFile rejects a GDAL-openable file with no MAP_PROJ attribute") {
    CHECK_THROWS_AS(WrfFile("tests/fixtures/not_a_wrf_file.tif"), UserError);
}

TEST_CASE("WrfFile reads a NetCDF4/HDF5-backed file identically to its classic-format original") {
    // wrfout_multitime_nc4.nc is `nccopy -k nc4` of wrfout_multitime.nc -
    // same data, HDF5-backed storage. Some real production WRF output is
    // NetCDF4/HDF5-backed, and on a machine whose GDAL netCDF driver isn't
    // built with HDF5 support, a bare (non-driver-forced) open would hand
    // such a file to GDAL's generic HDF5 driver instead, which exposes
    // variables so differently (attributes on band metadata instead of
    // dataset metadata, HDF5:"path"://VAR subdataset names) that every
    // variable used to get filtered out entirely. WrfFile always forces
    // the netCDF driver via the NETCDF: prefix specifically to avoid this -
    // see wrf_file.cpp's constructor comment.
    WrfFile classic("tests/fixtures/wrfout_multitime.nc");
    WrfFile nc4("tests/fixtures/wrfout_multitime_nc4.nc");
    CHECK(nc4.variables().size() == classic.variables().size());
    CHECK(nc4.read("T2", 1, 0) == classic.read("T2", 1, 0));
}

TEST_CASE("level index selects distinct data") {
    WrfFile file("tests/fixtures/wrfout_multitime.nc");
    std::set<long> roundedMeans;
    for (int level = 0; level < 3; ++level) {
        const auto values = file.read("U", 0, level);
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
        roundedMeans.insert(std::lround(mean * 1000));
    }
    CHECK(roundedMeans.size() == 3);
}

TEST_CASE("destaggering averages adjacent staggered cells") {
    // Reads U's raw (undestaggered) band directly via a separate GDAL
    // handle and checks WrfFile::read's destaggered output against the
    // textbook definition: the average of each pair of adjacent staggered
    // cells along the staggered axis.
    GDALAllRegister();
    const std::string target = "NETCDF:\"tests/fixtures/wrfout_multitime.nc\":U";
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> raw(
        static_cast<GDALDataset*>(GDALOpenEx(target.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    REQUIRE(raw);
    const int width = raw->GetRasterXSize(), height = raw->GetRasterYSize();
    std::vector<float> rawValues(static_cast<std::size_t>(width) * height);
    REQUIRE(raw->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, rawValues.data(), width, height, GDT_Float32, 0, 0) == CE_None);

    WrfFile file("tests/fixtures/wrfout_multitime.nc");
    const auto destaggered = file.read("U", 0, 0);
    REQUIRE(destaggered.size() == static_cast<std::size_t>(height) * (width - 1));
    for (int y = 0; y < height; ++y) for (int x = 0; x < width - 1; ++x) {
        const float expected = (rawValues[static_cast<std::size_t>(y) * width + x] + rawValues[static_cast<std::size_t>(y) * width + x + 1]) / 2.0f;
        CHECK(destaggered[static_cast<std::size_t>(y) * (width - 1) + x] == Catch::Approx(expected).epsilon(1e-5));
    }
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

// Ported from tests/test_crs_datum.py's
// test_root_domain_corner_lonlat_matches_sphere_datum, which exists to
// pin the WRF-sphere datum choice (radius 6370000 m, not WGS84) - see that
// file's module docstring for why a bbox CORNER, not the center, is the
// point that actually exercises the datum (a projection's own origin
// always round-trips to itself regardless of datum). These exact values
// are the Python suite's own pinned constants, not re-derived here.
TEST_CASE("root domain bbox corners match the Python reference (WRF sphere datum)") {
    auto project = readWpsNamelist("tests/fixtures/namelist_siblings.wps");
    project.domains.fillDomains();
    const auto& root = project.domains.domains().front();
    REQUIRE(root.bounds.has_value());
    const auto projection = project.domains.projection();
    const auto bottomLeft = projection.toLonLat({root.bounds->minX, root.bounds->minY});
    const auto topRight = projection.toLonLat({root.bounds->maxX, root.bounds->maxY});
    CHECK(bottomLeft.lon == Catch::Approx(3.112809).margin(1e-5));
    CHECK(bottomLeft.lat == Catch::Approx(43.905622).margin(1e-5));
    CHECK(topRight.lon == Catch::Approx(10.476400).margin(1e-5));
    CHECK(topRight.lat == Catch::Approx(49.014080).margin(1e-5));
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

namespace {
void checkBboxRoundTrip(const std::string& fixture) {
    auto original = readWpsNamelist(fixture);
    original.domains.fillDomains();
    const auto output = std::filesystem::temp_directory_path() / "wrftools-cpp-bbox-roundtrip.wps";
    writeWpsNamelist(original, output);
    auto reimported = readWpsNamelist(output);
    reimported.domains.fillDomains();
    std::filesystem::remove(output);

    const auto& originalDomains = original.domains.domains();
    const auto& reimportedDomains = reimported.domains.domains();
    REQUIRE(reimportedDomains.size() == originalDomains.size());
    for (std::size_t i = 0; i < originalDomains.size(); ++i) {
        CHECK(reimportedDomains[i].parentId == originalDomains[i].parentId);
        REQUIRE(originalDomains[i].bounds.has_value());
        REQUIRE(reimportedDomains[i].bounds.has_value());
        CHECK(reimportedDomains[i].bounds->minX == Catch::Approx(originalDomains[i].bounds->minX).margin(1.0));
        CHECK(reimportedDomains[i].bounds->minY == Catch::Approx(originalDomains[i].bounds->minY).margin(1.0));
        CHECK(reimportedDomains[i].bounds->maxX == Catch::Approx(originalDomains[i].bounds->maxX).margin(1.0));
        CHECK(reimportedDomains[i].bounds->maxY == Catch::Approx(originalDomains[i].bounds->maxY).margin(1.0));
    }
}
}  // namespace

TEST_CASE("sibling fixture export round-trips its domain bboxes") { checkBboxRoundTrip("tests/fixtures/namelist_siblings.wps"); }
TEST_CASE("linear-chain fixture export round-trips its domain bboxes") { checkBboxRoundTrip("tests/fixtures/namelist_hongkong.wps"); }

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

// Ported from tests/test_units.py's remaining cases not already covered
// above.
TEST_CASE("unit conversions: unrecognized/blank/category units offer only native") {
    CHECK(conversionsFor("some-made-up-unit").size() == 1);
    CHECK(conversionsFor("").size() == 1);
    CHECK(conversionsFor("category").size() == 1);
    CHECK(conversionsFor("some-made-up-unit").front().key == "native");
}

TEST_CASE("unit conversions: Kelvin to Fahrenheit boiling point") {
    CHECK(convert(373.15, findUnit("K", "degF")) == Catch::Approx(212.0));
}

TEST_CASE("unit conversions: m/s to knots matches the pinned conversion factor") {
    CHECK(convert(1.0, findUnit("m s-1", "kn")) == Catch::Approx(1.9438444924406046));
}

TEST_CASE("unit conversions: CF-style m/s spelling variants all normalize the same") {
    for (const std::string& spelling : {"m s-1", "m/s", "ms-1", "M S-1", "  m   s-1  "}) {
        const auto options = conversionsFor(spelling);
        std::vector<std::string> keys;
        for (const auto& option : options) keys.push_back(option.key);
        std::sort(keys.begin(), keys.end());
        CHECK(keys == std::vector<std::string>{"kmh", "kn", "mph", "native"});
    }
}

TEST_CASE("unit conversions: round-trip native to target and back is lossless") {
    const auto unit = findUnit("Pa", "hpa");
    const double nativeValue = 101325.0;
    const double converted = convert(nativeValue, unit);
    const double back = (converted - unit.offset) / unit.scale;
    CHECK(back == Catch::Approx(nativeValue));
}

// Ported from tests/test_colorbar.py's _format_tick cases - pure string
// formatting, no QApplication needed.
TEST_CASE("colorbar tick formatting matches the Python reference") {
    CHECK(formatColorbarTick(291.123456) == "291");
    CHECK(formatColorbarTick(0.000123456) == "0.000123");
    CHECK(formatColorbarTick(3.14159, "fixed", 2) == "3.14");
    CHECK(formatColorbarTick(3.14159, "fixed", 0) == "3");
    CHECK(formatColorbarTick(12345.0, "scientific", 2) == "1.23e+04");
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

// Ported from tests/test_colormaps.py's remaining cases not already
// covered above.
TEST_CASE("colormaps: unknown name is rejected") {
    CHECK_THROWS_AS(colormap("not-a-real-colormap"), std::out_of_range);
}

TEST_CASE("colormaps: degenerate range is fully transparent") {
    const auto& lut = colormap("viridis");
    const std::vector<float> values{1.0f, 2.0f};
    const auto image = applyColormap(values, 5.0f, 5.0f, lut);
    CHECK(image[0][3] == 0);
    CHECK(image[1][3] == 0);
}

TEST_CASE("colormaps: out-of-range values clip to the LUT ends") {
    const auto& lut = colormap("viridis");
    const std::vector<float> values{-100.0f, 100.0f};
    const auto image = applyColormap(values, 0.0f, 10.0f, lut);
    CHECK(Rgb{image[0][0], image[0][1], image[0][2]} == lut.front());
    CHECK(Rgb{image[1][0], image[1][1], image[1][2]} == lut.back());
}

TEST_CASE("colormaps: jet starts blue and ends red") {
    const auto& lut = colormap("jet");
    CHECK(lut.front()[2] > lut.front()[0]);  // blue > red at the low end
    CHECK(lut.back()[0] > lut.back()[2]);    // red > blue at the high end
}

TEST_CASE("categorical colormap: unknown-scheme fallback is deterministic across calls") {
    const auto first = categoricalLut("SOME_UNKNOWN_SCHEME", 1, 5);
    const auto second = categoricalLut("SOME_UNKNOWN_SCHEME", 1, 5);
    for (int value = 1; value <= 5; ++value) {
        CHECK(first.labels.at(value) == "Category " + std::to_string(value));
        CHECK(first.lut[static_cast<std::size_t>(value)] == second.lut[static_cast<std::size_t>(value)]);
    }
}

TEST_CASE("categorical colormap: out-of-LUT-range index is transparent") {
    const auto legend = categoricalLut("MODIFIED_IGBP_MODIS_NOAH", 1, 20);
    const std::vector<float> values{1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(), 999.0f};
    const auto pixels = applyCategoricalColormap(values, legend.lut);
    CHECK(Rgb{pixels[0][0], pixels[0][1], pixels[0][2]} == legend.lut[1]);
    CHECK(Rgb{pixels[1][0], pixels[1][1], pixels[1][2]} == legend.lut[2]);
    CHECK(pixels[2][3] == 0);  // NaN
    CHECK(pixels[3][3] == 0);  // out of LUT range
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

// --- LayerRenderer cache-tier hit/miss stats --------------------------------
// Ports test_rasterlayer.py's cache-behavior cases against LayerRenderer's
// stats() counters. Two Python cases have no C++ equivalent by design, not
// omission: overlay_for/effective_range/categorical_legend "of an unopened
// file returns None" - LayerRenderer::render() opens filePath on demand
// (WrfSourceRegistry::open is idempotent), it never distinguishes "not yet
// opened" from "open it now". RasterLayer.interpolate is likewise not part
// of RenderedRaster - it's applied by ViewForm when building the
// TileMapWidget overlay, one layer below LayerRenderer itself.

TEST_CASE("re-rendering the same layer hits the image cache") {
    LayerRenderer renderer;
    static_cast<void>(renderer.openFile({"tests/fixtures/geo_em_small.nc"}));
    const RasterLayer layer{.variable = "HGT_M"};
    static_cast<void>(renderer.render("tests/fixtures/geo_em_small.nc", layer));
    static_cast<void>(renderer.render("tests/fixtures/geo_em_small.nc", layer));
    CHECK(renderer.stats().sliceMisses == 1);
    CHECK(renderer.stats().sliceHits == 0);
    CHECK(renderer.stats().imageMisses == 1);
    CHECK(renderer.stats().imageHits == 1);
}

TEST_CASE("opacity change causes no new reads or images") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    RasterLayer layer{.variable = "HGT_M", .opacity = 0.5};
    static_cast<void>(renderer.render(path, layer));
    const auto before = renderer.stats();
    layer.opacity = 0.1;
    static_cast<void>(renderer.render(path, layer));
    CHECK(renderer.stats().sliceMisses == before.sliceMisses);
    CHECK(renderer.stats().sliceHits == before.sliceHits);
    CHECK(renderer.stats().imageMisses == before.imageMisses);
    CHECK(renderer.stats().imageHits == before.imageHits + 1);
}

TEST_CASE("colormap change misses the image cache only") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    RasterLayer layer{.variable = "HGT_M", .colormap = "viridis"};
    static_cast<void>(renderer.render(path, layer));
    const auto before = renderer.stats();
    layer.colormap = "plasma";
    static_cast<void>(renderer.render(path, layer));
    CHECK(renderer.stats().sliceMisses == before.sliceMisses);
    CHECK(renderer.stats().sliceHits == before.sliceHits + 1);
    CHECK(renderer.stats().imageMisses == before.imageMisses + 1);
}

TEST_CASE("time change misses both cache tiers") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    static_cast<void>(renderer.openFile({path}));
    RasterLayer layer{.variable = "T2", .timeIndex = 0};
    static_cast<void>(renderer.render(path, layer));
    const auto before = renderer.stats();
    layer.timeIndex = 1;
    static_cast<void>(renderer.render(path, layer));
    CHECK(renderer.stats().sliceMisses == before.sliceMisses + 1);
    CHECK(renderer.stats().imageMisses == before.imageMisses + 1);
}

TEST_CASE("stepping back in time is an image-cache hit") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    static_cast<void>(renderer.openFile({path}));
    RasterLayer layer{.variable = "T2", .timeIndex = 0};
    static_cast<void>(renderer.render(path, layer));
    layer.timeIndex = 1;
    static_cast<void>(renderer.render(path, layer));
    layer.timeIndex = 0;  // both its slice and image are still cached
    const auto before = renderer.stats();
    static_cast<void>(renderer.render(path, layer));
    CHECK(renderer.stats().sliceMisses == before.sliceMisses);
    CHECK(renderer.stats().sliceHits == before.sliceHits);
    CHECK(renderer.stats().imageHits == before.imageHits + 1);
}

TEST_CASE("prefetch populates the slice cache without building an image") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    static_cast<void>(renderer.openFile({path}));
    const RasterLayer layer{.variable = "T2", .timeIndex = 1};
    renderer.prefetch(path, layer);
    CHECK(renderer.stats().sliceMisses == 1);
    CHECK(renderer.stats().imageMisses == 0);
    const auto before = renderer.stats();
    static_cast<void>(renderer.render(path, layer));
    CHECK(renderer.stats().sliceMisses == before.sliceMisses);
    CHECK(renderer.stats().sliceHits == before.sliceHits + 1);
}

TEST_CASE("two layers on the same slice share the warp") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "HGT_M", .colormap = "viridis"}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "HGT_M", .colormap = "viridis"}));
    CHECK(renderer.stats().sliceMisses == 1);
}

TEST_CASE("invalidating a file does not affect another open file's cache") {
    LayerRenderer renderer;
    static_cast<void>(renderer.openFile({"tests/fixtures/geo_em_small.nc"}));
    static_cast<void>(renderer.openFile({"tests/fixtures/wrfout_multitime.nc"}));
    static_cast<void>(renderer.render("tests/fixtures/geo_em_small.nc", RasterLayer{.variable = "HGT_M"}));
    const RasterLayer wrfoutLayer{.variable = "T2"};
    static_cast<void>(renderer.render("tests/fixtures/wrfout_multitime.nc", wrfoutLayer));
    renderer.invalidateFile("tests/fixtures/geo_em_small.nc");
    const auto before = renderer.stats();
    static_cast<void>(renderer.render("tests/fixtures/wrfout_multitime.nc", wrfoutLayer));
    CHECK(renderer.stats().imageHits == before.imageHits + 1);  // untouched by the other file's invalidation
}

TEST_CASE("clear resets the cache stats") {
    LayerRenderer renderer;
    static_cast<void>(renderer.openFile({"tests/fixtures/geo_em_small.nc"}));
    static_cast<void>(renderer.render("tests/fixtures/geo_em_small.nc", RasterLayer{.variable = "HGT_M"}));
    renderer.clear();
    CHECK(renderer.stats().sliceMisses == 0);
    CHECK(renderer.stats().imageMisses == 0);
}

TEST_CASE("effective range honors a manual override and is auto otherwise") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    const auto autoRange = renderer.render(path, RasterLayer{.variable = "HGT_M"});
    CHECK(autoRange.minimum < autoRange.maximum);

    const auto manual = renderer.render(path, RasterLayer{.variable = "HGT_M", .minimum = 10.0f, .maximum = 200.0f});
    CHECK(manual.minimum == Catch::Approx(10.0));
    CHECK(manual.maximum == Catch::Approx(200.0));
}

TEST_CASE("effective range in a converted unit is shifted from native") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    static_cast<void>(renderer.openFile({path}));
    const auto native = renderer.render(path, RasterLayer{.variable = "T2"});
    const auto celsius = renderer.render(path, RasterLayer{.variable = "T2", .unitKey = "degC"});
    CHECK(celsius.minimum == Catch::Approx(native.minimum - 273.15).epsilon(1e-3));
    CHECK(celsius.maximum == Catch::Approx(native.maximum - 273.15).epsilon(1e-3));
}

TEST_CASE("manual range in a converted unit is used as-is") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    static_cast<void>(renderer.openFile({path}));
    const auto rendered = renderer.render(path, RasterLayer{.variable = "T2", .minimum = 0.0f, .maximum = 30.0f, .unitKey = "degC"});
    CHECK(rendered.minimum == Catch::Approx(0.0));
    CHECK(rendered.maximum == Catch::Approx(30.0));
}

TEST_CASE("units change misses the image cache but not the slice cache") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    static_cast<void>(renderer.openFile({path}));
    RasterLayer layer{.variable = "T2"};
    static_cast<void>(renderer.render(path, layer));
    const auto before = renderer.stats();
    layer.unitKey = "degC";
    static_cast<void>(renderer.render(path, layer));
    CHECK(renderer.stats().sliceMisses == before.sliceMisses);
    CHECK(renderer.stats().sliceHits == before.sliceHits + 1);
    CHECK(renderer.stats().imageMisses == before.imageMisses + 1);
}

TEST_CASE("two layers with the same slice but different units do not share the image cache") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/wrfout_multitime.nc";
    static_cast<void>(renderer.openFile({path}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "T2"}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "T2", .unitKey = "degC"}));
    CHECK(renderer.stats().sliceMisses == 1);
    CHECK(renderer.stats().imageMisses == 2);
}

TEST_CASE("tick settings do not invalidate either cache tier") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    RasterLayer layer{.variable = "HGT_M"};
    static_cast<void>(renderer.render(path, layer));
    const auto before = renderer.stats();
    layer.tickCount = 9; layer.tickFormat = "scientific"; layer.tickDecimals = 4;
    static_cast<void>(renderer.render(path, layer));
    CHECK(renderer.stats().sliceMisses == before.sliceMisses);
    CHECK(renderer.stats().sliceHits == before.sliceHits);
    CHECK(renderer.stats().imageMisses == before.imageMisses);
    CHECK(renderer.stats().imageHits == before.imageHits + 1);
}

TEST_CASE("LayerRenderer.openFile with a series registers one entry and dedupes on reopen") {
    LayerRenderer renderer;
    const std::vector<std::filesystem::path> seriesPaths{
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"};
    auto& first = renderer.openFile(seriesPaths);
    CHECK(renderer.openPaths() == std::vector<std::string>{seriesPaths.front().string()});
    auto& second = renderer.openFile(seriesPaths);
    CHECK(&first == &second);  // reopening the same series is a registry hit, not a rebuild
}

TEST_CASE("render works at every time index of a series") {
    LayerRenderer renderer;
    const std::vector<std::filesystem::path> seriesPaths{
        "tests/fixtures/wrfout_d01_2020-01-01_00_00_00.nc", "tests/fixtures/wrfout_d01_2020-01-01_00_30_00.nc",
        "tests/fixtures/wrfout_d01_2020-01-01_01_00_00.nc"};
    auto& source = renderer.openFile(seriesPaths);
    const auto times = source.seriesTimes();
    REQUIRE(times);
    for (int timeIndex = 0; timeIndex < static_cast<int>(times->size()); ++timeIndex) {
        const auto rendered = renderer.render(seriesPaths.front().string(), RasterLayer{.variable = "T2", .timeIndex = timeIndex});
        CHECK(rendered.width > 0);
        CHECK(rendered.height > 0);
    }
}

TEST_CASE("slice cache evicts by total bytes, not entry count") {
    LayerRenderer renderer(/*sliceCacheBytes=*/1, kDefaultImageCacheSize);
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "HGT_M"}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "LU_INDEX", .colormap = kCategoricalColormap}));
    CHECK(renderer.stats().sliceMisses == 2);
    // A different colormap forces an image-cache miss too, so this render
    // actually consults the slice tier instead of short-circuiting on an
    // image hit from the first render above - HGT_M's slice was evicted to
    // make room for LU_INDEX's, so this is a slice miss again.
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "HGT_M", .colormap = "plasma"}));
    CHECK(renderer.stats().sliceMisses == 3);
}

TEST_CASE("image cache evicts by entry count") {
    LayerRenderer renderer(kDefaultSliceCacheBytes, /*imageCacheSize=*/1);
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "HGT_M", .colormap = "viridis"}));
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "HGT_M", .colormap = "plasma"}));
    CHECK(renderer.stats().imageMisses == 2);
    // viridis's image was evicted to make room - re-rendering it is a miss again.
    static_cast<void>(renderer.render(path, RasterLayer{.variable = "HGT_M", .colormap = "viridis"}));
    CHECK(renderer.stats().imageMisses == 3);
}

TEST_CASE("LayerRenderer.render exposes the file's own landuse scheme for a categorical layer") {
    // geo_em_small.nc's MMINLU is MODIFIED_IGBP_MODIS_NOAH; its LU_INDEX
    // slice contains exactly classes 2, 13, 17 (verified against the fixture).
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    const auto rendered = renderer.render(path, RasterLayer{.variable = "LU_INDEX", .colormap = kCategoricalColormap});
    CHECK(rendered.presentCategories == std::vector<int>{2, 13, 17});
    CHECK(rendered.categoricalLabels.at(2) == "Evergreen Broadleaf Forest");
    CHECK(rendered.categoricalPalette[2] == Rgb{0x00, 0xFF, 0x00});
    CHECK(rendered.categoricalLabels.at(13) == "Urban and Built-Up");
    CHECK(rendered.categoricalPalette[13] == Rgb{0xFF, 0x00, 0x00});
    CHECK(rendered.categoricalLabels.at(17) == "Water");
    CHECK(rendered.categoricalPalette[17] == Rgb{0x00, 0x00, 0x80});
}

TEST_CASE("a continuous layer's render carries no categorical legend data") {
    LayerRenderer renderer;
    const std::string path = "tests/fixtures/geo_em_small.nc";
    static_cast<void>(renderer.openFile({path}));
    const auto rendered = renderer.render(path, RasterLayer{.variable = "HGT_M"});
    CHECK(rendered.presentCategories.empty());
    CHECK(rendered.categoricalLabels.empty());
}

TEST_CASE("isWpsGeogDataset recognizes a directory with an index file, not a plain file or directory") {
    CHECK(isWpsGeogDataset("tests/fixtures/geotiff_convert/wps_soiltemp_1deg"));
    CHECK_FALSE(isWpsGeogDataset("tests/fixtures/geotiff_convert/wps_soiltemp_1deg/index"));
    CHECK_FALSE(isWpsGeogDataset("tests/fixtures/geotiff_convert/utm.tif"));
    CHECK_FALSE(isWpsGeogDataset("tests/fixtures/does-not-exist"));
}

TEST_CASE("WpsBinarySource reads a real WPS_GEOG regular_ll dataset with the correct geometry") {
    // tests/fixtures/geotiff_convert/wps_soiltemp_1deg/index: regular_ll,
    // dx=dy=1.0 degree, known_x=known_y=1.0 (an integer -> WPS/GEOGRID's
    // cell-center convention) at known_lon=-179.5/known_lat=-89.5 - the
    // real center of the southwesternmost 1-degree cell of a global grid,
    // so the raster's true edges are exactly -180/-90 (west/south), not
    // -179.5/-89.5.
    WpsBinarySource source("tests/fixtures/geotiff_convert/wps_soiltemp_1deg");
    CHECK(source.size() == std::array<int, 2>{180, 180});
    CHECK(source.displayName() == "wps_soiltemp_1deg");
    const auto& gt = source.geotransform();
    CHECK(gt[0] == Catch::Approx(-180.0));
    CHECK(gt[1] == Catch::Approx(1.0));
    CHECK(gt[2] == Catch::Approx(0.0));
    CHECK(gt[3] == Catch::Approx(90.0));
    CHECK(gt[4] == Catch::Approx(0.0));
    CHECK(gt[5] == Catch::Approx(-1.0));
    REQUIRE(source.variables().size() == 1);
    const auto& variable = source.variables().front();
    CHECK(variable.name == "Annual mean deep soil temperature");
    CHECK(variable.units == "Kelvin");
    CHECK(variable.levelCount == 1);
    CHECK_FALSE(variable.categoryScheme.has_value());
    const auto values = source.read(variable.name, 0, 0);
    REQUIRE(values.size() == 180 * 180);
    // A row-1 (tile-file numbering, south) sample: dy is non-negative so
    // read() must have flipped it up to the last (northmost-index) output
    // row - CHECK it lands there, not at row 0.
    const bool anyFiniteInLastRow = std::any_of(values.end() - 180, values.end(), [](float v) { return std::isfinite(v); });
    CHECK(anyFiniteInLastRow);
}

TEST_CASE("WrfSourceRegistry opens a WPS_GEOG directory as a WpsBinarySource, not a NetCDF file") {
    WrfSourceRegistry registry;
    auto& source = registry.open({"tests/fixtures/geotiff_convert/wps_soiltemp_1deg"});
    CHECK(source.variables().size() == 1);
    CHECK(source.displayName() == "wps_soiltemp_1deg");
}
