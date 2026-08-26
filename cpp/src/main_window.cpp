#include "wrftools/main_window.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/view_form.hpp"
#include "wrftools/domain_form.hpp"
#include "wrftools/geotiff_convert_form.hpp"

#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>

namespace wrftools {
namespace {
// One scroll area per tab (not one wrapping the QTabWidget) so each panel
// scrolls independently and the tab bar itself stays pinned - mirrors
// app.py's WhiteScroll. Without this, a tab's QVBoxLayout of stacked
// QGroupBoxes has nowhere to go once its natural height exceeds the space
// the splitter happens to give it: Qt still lays every row out at its full
// size, but the widget's own allocated rect gets shrunk to fit, so rows
// end up drawn on top of each other - resizing the window bigger "fixes"
// it only because there's then enough room.
QScrollArea* scrollWrap(QWidget* content) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    return scroll;
}
}  // namespace

MainWindow::MainWindow() {
    setWindowTitle("WRF Tools");
    resize(1300, 800);
    auto* tabs = new QTabWidget;
    tabs->setMinimumWidth(340);
    map_ = new TileMapWidget;
    auto* domainForm = new DomainForm(map_);
    tabs->addTab(scrollWrap(domainForm), "Domains");
    tabs->addTab(scrollWrap(new ViewForm(map_)), "View");
    tabs->addTab(scrollWrap(new GeotiffConvertForm), "Convert");
    // Only the active tab's redraw should recenter the shared map - without
    // this, e.g. every Domains-tab field edit would yank the camera back
    // while the user is looking at View-tab raster layers.
    connect(tabs, &QTabWidget::currentChanged, this, [domainForm, tabs](int index) { domainForm->setActive(tabs->widget(index) == domainForm); });
    auto* splitter = new QSplitter;
    splitter->addWidget(tabs);
    splitter->addWidget(map_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 940});
    setCentralWidget(splitter);
    auto* action = menuBar()->addMenu("&File")->addAction("Export Map Image...");
    action->setShortcut(QKeySequence::SaveAs);
    connect(action, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export map image as", "map.png", "PNG Image (*.png);;JPEG Image (*.jpg *.jpeg)");
        if (!path.isEmpty()) static_cast<void>(map_->exportImage(path));
    });
}
}  // namespace wrftools
