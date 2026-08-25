// UI-level tests (real Qt widget interaction) for view_form/domain_form/main_window.
// Runs headless via QT_QPA_PLATFORM=offscreen, set by CMakeLists.txt for this target.
// WORKING_DIRECTORY is the repo root (also set by CMakeLists.txt), so
// fixtures resolve as "tests/fixtures/...".
#include "wrftools/domain_form.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/wps_namelist.hpp"

#include <QApplication>
#include <QTreeWidget>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wrftools;

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}

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
