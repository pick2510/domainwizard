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
class QSlider;
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

    // Action methods a real button click would invoke - public (not just
    // reachable via a QTest::mouseClick on the button itself) so tests can
    // drive them directly, matching how the Python tests call
    // form.on_remove_layer_button_clicked() etc.
    void closeSelectedFile();
    void removeSelectedLayer();
    // direction: +1 moves the selected layer up (later in draw order,
    // matching moveUpButton_'s wiring), -1 moves it down.
    void moveSelectedLayer(int direction);
    void zoomToSelectedLayer();
    [[nodiscard]] std::optional<int> selectedLayerId() const noexcept { return selectedLayerId_; }

    // Test-facing widget accessors only - mirror the public attributes
    // Python's ViewForm tests reach directly (form.variable_combo, etc.);
    // production code (this class's own .cpp) never needs these, it holds
    // the pointers directly.
    [[nodiscard]] QPushButton* addLayerButton() const noexcept { return addLayerButton_; }
    [[nodiscard]] QPushButton* closeFileButton() const noexcept { return closeFileButton_; }
    [[nodiscard]] QPushButton* removeLayerButton() const noexcept { return removeLayerButton_; }
    [[nodiscard]] QPushButton* moveUpButton() const noexcept { return moveUpButton_; }
    [[nodiscard]] QPushButton* moveDownButton() const noexcept { return moveDownButton_; }
    [[nodiscard]] QWidget* propertiesGroup() const noexcept { return propertiesGroup_; }
    [[nodiscard]] QComboBox* variableCombo() const noexcept { return variable_; }
    [[nodiscard]] QComboBox* timeCombo() const noexcept { return time_; }
    [[nodiscard]] QSpinBox* levelSpin() const noexcept { return level_; }
    [[nodiscard]] QLabel* levelLabel() const noexcept { return levelLabel_; }
    [[nodiscard]] QComboBox* colormapCombo() const noexcept { return colormap_; }
    [[nodiscard]] QComboBox* unitsCombo() const noexcept { return units_; }
    [[nodiscard]] QSlider* opacitySlider() const noexcept { return opacity_; }
    [[nodiscard]] QCheckBox* autoRangeCheck() const noexcept { return autoRange_; }
    [[nodiscard]] QDoubleSpinBox* minimumSpin() const noexcept { return minimum_; }
    [[nodiscard]] QDoubleSpinBox* maximumSpin() const noexcept { return maximum_; }
    [[nodiscard]] QCheckBox* interpolateCheck() const noexcept { return interpolate_; }
    [[nodiscard]] QCheckBox* playCheck() const noexcept { return play_; }
    [[nodiscard]] QTimer* playbackTimer() const noexcept { return playbackTimer_; }
    [[nodiscard]] QPushButton* previousStepButton() const noexcept { return previousStepButton_; }
    [[nodiscard]] QPushButton* nextStepButton() const noexcept { return nextStepButton_; }
    [[nodiscard]] QSpinBox* playIntervalSpin() const noexcept { return playInterval_; }
    // Steps the time combo by `direction` (+1/-1), wrapping at either end -
    // what a real playback tick, or the Previous/Next step buttons, does.
    // Public (not just reachable by waiting for playbackTimer()'s real
    // interval, or a real button click) so tests can drive it directly.
    void stepPlayback(int direction);
    // Equivalent to stepPlayback(1) - kept as its own method since it's
    // what playbackTimer()'s timeout signal calls, matching viewform.py's
    // _advance_play().
    void advancePlayback();
    [[nodiscard]] QSpinBox* tickCountSpin() const noexcept { return tickCount_; }
    [[nodiscard]] QComboBox* tickFormatCombo() const noexcept { return tickFormat_; }
    [[nodiscard]] QSpinBox* tickDecimalsSpin() const noexcept { return tickDecimals_; }
    [[nodiscard]] QCheckBox* showInfoCheck() const noexcept { return showInfo_; }
    [[nodiscard]] QCheckBox* northArrowCheck() const noexcept { return northArrow_; }

    // Applies the properties-panel widgets' current values to the selected
    // layer and re-renders. Public because the range fields' validation
    // (max must exceed min) throws UserError directly here, matching
    // viewform.py's on_range_changed - callers driven by a real Qt signal
    // catch it via applyFieldsFromSignal() below instead.
    void applyFieldsToSelectedLayer();

private:
    void openFile();
    void openGeogDataset();
    void applyFieldsFromSignal();
    void rebuildFileList(const std::optional<std::string>& selectPath = std::nullopt);
    [[nodiscard]] std::optional<std::string> selectedFilePath() const;
    [[nodiscard]] std::vector<std::string> openFilePaths() const;

    void rebuildLayerTree(std::optional<int> selectId = std::nullopt);
    [[nodiscard]] std::optional<std::size_t> selectedLayerIndex() const;
    [[nodiscard]] ViewLayer* selectedLayer();
    void onLayerSelectionChanged();
    void onLayerCheckStateChanged(QTreeWidgetItem* item);

    void updatePanelVisibility();
    void populatePropertiesPanel();
    void onVariableChanged();
    void onTickSettingsChanged();
    // Given the lon/lat under the cursor, returns a formatted "value (unit)"
    // string sampled from the topmost visible layer, or nullopt if no
    // visible layer covers that point (or its pixel there is nodata) - fed
    // to TileMapWidget's hover-value handler, registered once in the
    // constructor.
    [[nodiscard]] std::optional<QString> hoverValueAt(LonLat point);

    void refreshMap();
    void updateColorbar();
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
    QPushButton* addLayerButton_{};
    QPushButton* removeLayerButton_{};
    QPushButton* moveUpButton_{};
    QPushButton* moveDownButton_{};
    QWidget* propertiesGroup_{};
    QWidget* colorbarGroup_{};
    QWidget* zoomGroup_{};

    QComboBox* variable_{};
    QComboBox* time_{};
    QSpinBox* level_{};
    QLabel* levelLabel_{};
    QComboBox* colormap_{};
    QComboBox* units_{};
    QSlider* opacity_{};
    QLabel* opacityLabel_{};
    QCheckBox* autoRange_{};
    QDoubleSpinBox* minimum_{};
    QDoubleSpinBox* maximum_{};
    QCheckBox* interpolate_{};
    QCheckBox* play_{};
    QTimer* playbackTimer_{};
    QPushButton* previousStepButton_{};
    QPushButton* nextStepButton_{};
    QSpinBox* playInterval_{};

    QSpinBox* tickCount_{};
    QComboBox* tickFormat_{};
    QSpinBox* tickDecimals_{};
    QCheckBox* showInfo_{};
    QCheckBox* northArrow_{};

    QLabel* preview_{};
    QLabel* status_{};
};
}  // namespace wrftools
