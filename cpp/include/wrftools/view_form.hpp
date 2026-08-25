#pragma once

#include "wrftools/layer_renderer.hpp"
#include "wrftools/raster_layer.hpp"

#include <optional>
#include <string>
#include <vector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace wrftools {
class TileMapWidget;

// One entry in the layer stack: which open file/series it reads from, plus
// its own independent render settings. Mirrors rasterlayer.RasterLayer,
// split from the render-parameter RasterLayer struct (raster_layer.hpp)
// because filePath/layerId are ViewForm bookkeeping, not render inputs.
struct ViewLayer {
    int layerId{};
    std::string filePath;
    RasterLayer settings;
};

class ViewForm final : public QWidget {
public:
    explicit ViewForm(TileMapWidget* map, QWidget* parent = nullptr);

    // Opens files/series directly (bypassing the file-picker openFile()
    // uses) - the entry point both openFile() and tests use. Mirrors
    // DomainForm::setProject's role for the Domains tab.
    void openFiles(const std::vector<std::string>& paths);
    void addLayer();
    [[nodiscard]] const std::vector<ViewLayer>& layers() const noexcept { return layers_; }
    [[nodiscard]] QTreeWidget* fileTreeWidget() const noexcept { return fileTree_; }
    [[nodiscard]] QTreeWidget* layerTreeWidget() const noexcept { return layerTree_; }

private:
    void openFile();
    void closeSelectedFile();
    void rebuildFileList(const std::optional<std::string>& selectPath = std::nullopt);
    [[nodiscard]] std::optional<std::string> selectedFilePath() const;
    [[nodiscard]] std::vector<std::string> openFilePaths() const;

    void removeSelectedLayer();
    void moveSelectedLayer(int direction);
    void rebuildLayerTree(std::optional<int> selectId = std::nullopt);
    [[nodiscard]] std::optional<std::size_t> selectedLayerIndex() const;
    [[nodiscard]] ViewLayer* selectedLayer();
    void onLayerSelectionChanged();
    void onLayerCheckStateChanged(QTreeWidgetItem* item);

    void updatePanelVisibility();
    void populatePropertiesPanel();
    void onVariableChanged();
    void applyFieldsToSelectedLayer();

    void refreshMap();
    void updateColorbar();
    void zoomToSelectedLayer();
    [[nodiscard]] std::string layerLabel(const ViewLayer& layer);

    TileMapWidget* map_;
    LayerRenderer renderer_;
    std::vector<ViewLayer> layers_;  // bottom-first: draw order
    int nextLayerId_{1};
    std::optional<int> selectedLayerId_;
    bool hasAutoZoomed_{false};
    bool updatingLayerTree_{false};

    QTreeWidget* fileTree_{};
    QPushButton* closeFileButton_{};
    QTreeWidget* layerTree_{};
    QPushButton* removeLayerButton_{};
    QPushButton* moveUpButton_{};
    QPushButton* moveDownButton_{};
    QWidget* propertiesGroup_{};
    QWidget* zoomGroup_{};

    QComboBox* variable_{};
    QComboBox* time_{};
    QSpinBox* level_{};
    QLabel* levelLabel_{};
    QComboBox* colormap_{};
    QComboBox* units_{};
    QDoubleSpinBox* opacity_{};
    QCheckBox* autoRange_{};
    QDoubleSpinBox* minimum_{};
    QDoubleSpinBox* maximum_{};
    QCheckBox* interpolate_{};
    QCheckBox* play_{};
    QTimer* playbackTimer_{};

    QLabel* preview_{};
    QLabel* status_{};
};
}  // namespace wrftools
