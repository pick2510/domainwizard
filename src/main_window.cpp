#include "wrftools/main_window.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/theme.hpp"
#include "wrftools/view_form.hpp"
#include "wrftools/domain_form.hpp"
#include "wrftools/geotiff_convert_form.hpp"
#include "wrftools/lcz_form.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
// QStyleHints::colorScheme()/colorSchemeChanged only exist from Qt 6.5;
// this project's CMake minimum stays at 6.4 (see theme.hpp's ColorScheme),
// so live OS-theme tracking is only compiled in when actually available.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

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
    tabs->addTab(scrollWrap(new LczForm), "LCZ");
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
    auto* filemenu = menuBar()->addMenu("&File");
    auto* action = filemenu->addAction("Export Map Image...");
    auto* close = filemenu->addAction("Close");
    close->setShortcut(QKeySequence::Quit);
    action->setShortcut(QKeySequence::SaveAs);
    connect(action, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export map image as", "map.png", "PNG Image (*.png);;JPEG Image (*.jpg *.jpeg)");
        if (!path.isEmpty()) static_cast<void>(map_->exportImage(path));
    });
    connect(close, &QAction::triggered, this, &QCoreApplication::quit);

    // Options > Theme: System follows the OS light/dark setting (see
    // theme.hpp's resolveColorScheme/applyColorScheme - GNOME's own
    // gsettings preference wins there over Qt's own, often-unreliable
    // QStyleHints::colorScheme()); Light/Dark are an explicit override
    // that sticks regardless of what the OS reports afterward, for a
    // desktop/Qt combination where even that detection can't be trusted,
    // or a user who simply prefers a fixed choice. Persisted via
    // QSettings so it survives a restart.
    auto* application = qobject_cast<QApplication*>(QApplication::instance());
    // The only place Qt::ColorScheme itself is touched - everything else
    // in this file and in theme.hpp/.cpp works with wrftools::ColorScheme
    // instead, so only this one call site needs the Qt 6.5 guard.
    auto systemColorScheme = [application]() -> ColorScheme {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (application) {
            switch (application->styleHints()->colorScheme()) {
                case Qt::ColorScheme::Dark: return ColorScheme::Dark;
                case Qt::ColorScheme::Light: return ColorScheme::Light;
                default: return ColorScheme::Unknown;
            }
        }
#endif
        return ColorScheme::Unknown;
    };
    auto applyPreference = [application, systemColorScheme](ThemePreference preference) {
        if (!application) return;
        switch (preference) {
            case ThemePreference::Light: applyColorScheme(*application, ColorScheme::Light); break;
            case ThemePreference::Dark: applyColorScheme(*application, ColorScheme::Dark); break;
            case ThemePreference::System: applyColorScheme(*application, resolveColorScheme(systemColorScheme())); break;
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
    // of what the OS does afterward. Only available when built against
    // Qt 6.5+ (see the #include guard above); on 6.4, System still works
    // via applyPreference() above, just without live tracking - the next
    // toggle of systemThemeAction_ (or app restart) re-resolves it.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (application) {
        connect(application->styleHints(), &QStyleHints::colorSchemeChanged, this, [this, applyPreference](Qt::ColorScheme) {
            if (systemThemeAction_->isChecked()) applyPreference(ThemePreference::System);
        });
    }
#endif
}
}  // namespace wrftools
