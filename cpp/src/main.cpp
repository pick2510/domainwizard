#include "wrftools/main_window.hpp"

#include <QApplication>
#include <gdal.h>

int main(int argc, char* argv[]) {
    GDALAllRegister();
    QApplication application(argc, argv);
    application.setApplicationName("WRF Tools");
    application.setOrganizationName("WRF Tools");
    wrftools::MainWindow window;
    window.show();
    return application.exec();
}
