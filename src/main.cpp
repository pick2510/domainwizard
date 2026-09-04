#include "wrftools/main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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

    // Theme (follow OS light/dark, or an explicit Options > Theme
    // override) is applied by MainWindow itself, as early as possible in
    // its constructor - see main_window.cpp - since it needs the
    // persisted Options > Theme preference (QSettings, using the
    // organization/application name just set above) to decide between
    // System/Light/Dark, not just the OS's own reported scheme.
    configureGdalData();
    GDALAllRegister();
    // resources.qrc (the About dialog's logo) is compiled into wrftools_ui,
    // a *static* library - its auto-generated resource-registration code
    // is a translation unit nothing else references, so the linker drops
    // it entirely unless something forces it in. Q_INIT_RESOURCE (the
    // linker-visible symbol it expands to) is that force; it must run at
    // global scope, not inside the wrftools namespace, or it names a
    // wrftools::qInitResources_resources() that was never linked in either.
    Q_INIT_RESOURCE(resources);
    wrftools::MainWindow window;
    window.show();
    return application.exec();
}
