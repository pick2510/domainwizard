#include "wrftools/main_window.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/theme.hpp"
#include "wrftools/view_form.hpp"
#include "wrftools/domain_form.hpp"
#include "wrftools/geotiff_convert_form.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStyleHints>
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

    // Options > Theme: System follows the OS light/dark setting (see
    // theme.hpp's resolveColorScheme/applyColorScheme - GNOME's own
    // gsettings preference wins there over Qt's own, often-unreliable
    // QStyleHints::colorScheme()); Light/Dark are an explicit override
    // that sticks regardless of what the OS reports afterward, for a
    // desktop/Qt combination where even that detection can't be trusted,
    // or a user who simply prefers a fixed choice. Persisted via
    // QSettings so it survives a restart.
    auto* application = qobject_cast<QApplication*>(QApplication::instance());
    auto applyPreference = [application](ThemePreference preference) {
        if (!application) return;
        switch (preference) {
            case ThemePreference::Light: applyColorScheme(*application, Qt::ColorScheme::Light); break;
            case ThemePreference::Dark: applyColorScheme(*application, Qt::ColorScheme::Dark); break;
            case ThemePreference::System: applyColorScheme(*application, resolveColorScheme(application->styleHints()->colorScheme())); break;
        }
    };

    auto* themeMenu = menuBar()->addMenu("&Options")->addMenu("Theme");
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    systemThemeAction_ = themeMenu->addAction("System");
    lightThemeAction_ = themeMenu->addAction("Light");
    darkThemeAction_ = themeMenu->addAction("Dark");
    for (auto* themeAction : {systemThemeAction_, lightThemeAction_, darkThemeAction_}) {
        themeAction->setCheckable(true);
        themeGroup->addAction(themeAction);
    }

    QSettings settings;
    const auto initialPreference = themePreference(settings);
    (initialPreference == ThemePreference::Light   ? lightThemeAction_
            : initialPreference == ThemePreference::Dark ? darkThemeAction_
                                                          : systemThemeAction_)
        ->setChecked(true);
    applyPreference(initialPreference);

    connect(systemThemeAction_, &QAction::triggered, this, [applyPreference] {
        QSettings s;
        setThemePreference(s, ThemePreference::System);
        applyPreference(ThemePreference::System);
    });
    connect(lightThemeAction_, &QAction::triggered, this, [applyPreference] {
        QSettings s;
        setThemePreference(s, ThemePreference::Light);
        applyPreference(ThemePreference::Light);
    });
    connect(darkThemeAction_, &QAction::triggered, this, [applyPreference] {
        QSettings s;
        setThemePreference(s, ThemePreference::Dark);
        applyPreference(ThemePreference::Dark);
    });

    // Live OS-theme tracking only matters while System is the active
    // preference - an explicit Light/Dark choice should stick regardless
    // of what the OS does afterward.
    if (application) {
        connect(application->styleHints(), &QStyleHints::colorSchemeChanged, this, [this, application](Qt::ColorScheme scheme) {
            if (systemThemeAction_->isChecked()) applyColorScheme(*application, resolveColorScheme(scheme));
        });
    }
}
}  // namespace wrftools
