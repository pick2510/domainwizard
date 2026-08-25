#pragma once

#include <QMainWindow>

namespace wrftools {
class TileMapWidget;

class MainWindow final : public QMainWindow {
public:
    MainWindow();

private:
    TileMapWidget* map_;
};
}  // namespace wrftools
