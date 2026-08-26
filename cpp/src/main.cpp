#include "wrftools/main_window.hpp"
#include "wrftools/theme.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStyleHints>
#include <gdal.h>

namespace {
void configureGdalData() {
    // Packagers may place GDAL/PROJ data beside the executable. Do not
    // override explicit user or Homebrew/system configuration.
    const auto directory = QCoreApplication::applicationDirPath();
    const auto gdal = QDir(directory).filePath("../share/gdal");
    const auto proj = QDir(directory).filePath("../share/proj");
    if (qEnvironmentVariableIsEmpty("GDAL_DATA") && QDir(gdal).exists()) qputenv("GDAL_DATA", gdal.toUtf8());
    if (qEnvironmentVariableIsEmpty("PROJ_DATA") && QDir(proj).exists()) qputenv("PROJ_DATA", proj.toUtf8());
}

// Some prebuilt Qt distributions (e.g. aqtinstall's Linux packages) ship
// with fontconfig compiled out, so QPlatformFontDatabase falls back to a
// private "<qtdir>/lib/fonts" directory instead of the system font stack -
// if that directory was never deployed, Qt silently falls back to a single
// built-in "last resort" font with broken/garbled glyph rendering for
// anything beyond simple digits (this is what made e.g. a QComboBox reading
// "Lambert Conformal" render as overlapping, unreadable strokes). Must run
// BEFORE QApplication is constructed - the platform plugin reads
// QT_QPA_FONTDIR while initializing, not lazily on first font use.
void configureFontDir(const char* argv0) {
    if (!qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) return;  // respect explicit user/packager configuration
    const auto directory = QFileInfo(QString::fromLocal8Bit(argv0)).absolutePath();
    const auto bundled = QDir(directory).filePath("../share/fonts");
    if (QDir(bundled).exists()) { qputenv("QT_QPA_FONTDIR", bundled.toUtf8()); return; }
    for (const char* candidate : {"/usr/share/fonts", "/usr/local/share/fonts"}) {
        if (QDir(candidate).exists()) { qputenv("QT_QPA_FONTDIR", candidate); return; }
    }
}
}

int main(int argc, char* argv[]) {
    configureFontDir(argc > 0 ? argv[0] : "");
    QApplication application(argc, argv);
    application.setApplicationName("WRF Tools");
    application.setOrganizationName("WRF Tools");

    // Follow the OS light/dark setting rather than always the platform's
    // default (light) look - this must be the very first call, before any
    // other style/palette change, since applyColorScheme's first call
    // captures the untouched native style/palette to revert to later.
    // Live-updates too: styleHints() outlives the application, so this
    // connection needs no explicit disconnect.
    wrftools::applyColorScheme(application, application.styleHints()->colorScheme());
    QObject::connect(application.styleHints(), &QStyleHints::colorSchemeChanged, &application,
        [&application](Qt::ColorScheme scheme) { wrftools::applyColorScheme(application, scheme); });

    configureGdalData();
    GDALAllRegister();
    wrftools::MainWindow window;
    window.show();
    return application.exec();
}
