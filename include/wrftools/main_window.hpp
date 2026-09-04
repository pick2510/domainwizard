#pragma once

#include <QMainWindow>

class QAction;

namespace wrftools {
class TileMapWidget;

class MainWindow final : public QMainWindow {
public:
    MainWindow();

    // Test-facing accessors only - production code (this class's own
    // constructor) never needs these, it holds the pointers directly.
    [[nodiscard]] QAction* systemThemeAction() const noexcept { return systemThemeAction_; }
    [[nodiscard]] QAction* lightThemeAction() const noexcept { return lightThemeAction_; }
    [[nodiscard]] QAction* darkThemeAction() const noexcept { return darkThemeAction_; }

private:
    void showAboutDialog();

    TileMapWidget* map_;
    QAction* systemThemeAction_{};
    QAction* lightThemeAction_{};
    QAction* darkThemeAction_{};
};
}  // namespace wrftools
