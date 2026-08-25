#include "wrftools/main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
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
}

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName("WRF Tools");
    application.setOrganizationName("WRF Tools");
    configureGdalData();
    GDALAllRegister();
    wrftools::MainWindow window;
    window.show();
    return application.exec();
}
