#include "wrftools/main_window.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/view_form.hpp"

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
    tabs->addTab(new QLabel("Domains are being ported to C++.", tabs), "Domains");
    map_ = new TileMapWidget;
    tabs->addTab(new ViewForm(map_, tabs), "View");
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
