#include "wrftools/main_window.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/view_form.hpp"
#include "wrftools/domain_form.hpp"

#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QSplitter>
#include <QTabWidget>

namespace wrftools {
MainWindow::MainWindow() {
    setWindowTitle("WRF Tools");
    resize(1300, 800);
    auto* tabs = new QTabWidget;
    tabs->setMinimumWidth(340);
    map_ = new TileMapWidget;
    auto* domainForm = new DomainForm(map_, tabs);
    tabs->addTab(domainForm, "Domains");
    tabs->addTab(new ViewForm(map_, tabs), "View");
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
