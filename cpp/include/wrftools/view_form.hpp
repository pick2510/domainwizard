#pragma once

#include <QWidget>
#include <memory>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QCheckBox;
class QTimer;
class QDoubleSpinBox;

namespace wrftools { class WrfFile; class TileMapWidget; }

namespace wrftools {
class ViewForm final : public QWidget {
public:
    explicit ViewForm(TileMapWidget* map, QWidget* parent = nullptr);
private:
    void openFile();
    void closeFile();
    void refreshVariables();
    void renderSelected();
    std::unique_ptr<WrfFile> file_;
    TileMapWidget* map_{};
    bool hasAutoZoomed_{false};
    QComboBox* variable_{};
    QComboBox* colormap_{};
    QComboBox* units_{};
    QSpinBox* time_{};
    QSpinBox* level_{};
    QCheckBox* play_{};
    QCheckBox* visible_{};
    QTimer* playbackTimer_{};
    QDoubleSpinBox* opacity_{};
    QDoubleSpinBox* minimum_{};
    QDoubleSpinBox* maximum_{};
    QLabel* preview_{};
    QLabel* status_{};
};
}  // namespace wrftools
