#pragma once

#include <QWidget>
#include <memory>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace wrftools { class WrfFile; class TileMapWidget; }

namespace wrftools {
class ViewForm final : public QWidget {
public:
    explicit ViewForm(TileMapWidget* map, QWidget* parent = nullptr);
private:
    void openFile();
    void refreshVariables();
    void renderSelected();
    std::unique_ptr<WrfFile> file_;
    TileMapWidget* map_{};
    QComboBox* variable_{};
    QComboBox* colormap_{};
    QComboBox* units_{};
    QSpinBox* time_{};
    QSpinBox* level_{};
    QLabel* preview_{};
    QLabel* status_{};
};
}  // namespace wrftools
