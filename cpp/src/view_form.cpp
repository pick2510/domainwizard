#include "wrftools/view_form.hpp"

#include "wrftools/colorbar.hpp"
#include "wrftools/colormaps.hpp"
#include "wrftools/error.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/units.hpp"
#include "wrftools/wrf_series.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace wrftools {
namespace {
constexpr int kLayerIdRole = Qt::UserRole;
constexpr double kMercatorEarthRadius = 6378137.0;

LonLat mercatorToLonLat(double x, double y) {
    return {x / kMercatorEarthRadius * 180.0 / M_PI, std::atan(std::sinh(y / kMercatorEarthRadius)) * 180.0 / M_PI};
}
}  // namespace

ViewForm::ViewForm(TileMapWidget* map, QWidget* parent) : QWidget(parent), map_(map) {
    auto* layout = new QVBoxLayout(this);

    auto* filesGroup = new QGroupBox("Files", this);
    auto* filesLayout = new QVBoxLayout;
    fileTree_ = new QTreeWidget(this); fileTree_->setHeaderHidden(true);
    auto* openFileButton = new QPushButton("Open WRF/WPS NetCDF…", this);
    closeFileButton_ = new QPushButton("Close File", this); closeFileButton_->setEnabled(false);
    filesLayout->addWidget(fileTree_); filesLayout->addWidget(openFileButton); filesLayout->addWidget(closeFileButton_);
    filesGroup->setLayout(filesLayout);
    layout->addWidget(filesGroup);

    auto* layersGroup = new QGroupBox("Layers", this);
    auto* layersLayout = new QVBoxLayout;
    layerTree_ = new QTreeWidget(this); layerTree_->setHeaderHidden(true);
    auto* addLayerButton = new QPushButton("Add Layer", this);
    removeLayerButton_ = new QPushButton("Remove Layer", this); removeLayerButton_->setEnabled(false);
    auto* moveLayout = new QHBoxLayout;
    moveUpButton_ = new QPushButton("Move Up", this); moveDownButton_ = new QPushButton("Move Down", this);
    moveUpButton_->setEnabled(false); moveDownButton_->setEnabled(false);
    moveLayout->addWidget(moveUpButton_); moveLayout->addWidget(moveDownButton_);
    layersLayout->addWidget(layerTree_); layersLayout->addWidget(addLayerButton); layersLayout->addWidget(removeLayerButton_); layersLayout->addLayout(moveLayout);
    layersGroup->setLayout(layersLayout);
    layout->addWidget(layersGroup);

    propertiesGroup_ = new QGroupBox("Layer Properties", this);
    auto* form = new QFormLayout;
    variable_ = new QComboBox(this); colormap_ = new QComboBox(this); units_ = new QComboBox(this); time_ = new QComboBox(this);
    level_ = new QSpinBox(this); level_->setMinimum(1);
    play_ = new QCheckBox("Play", this); playbackTimer_ = new QTimer(this);
    opacity_ = new QDoubleSpinBox(this); opacity_->setRange(0.0, 1.0); opacity_->setSingleStep(0.1); opacity_->setValue(0.8);
    autoRange_ = new QCheckBox("Auto range", this); autoRange_->setChecked(true);
    minimum_ = new QDoubleSpinBox(this); maximum_ = new QDoubleSpinBox(this);
    for (auto* range : {minimum_, maximum_}) { range->setRange(-1e30, 1e30); range->setDecimals(4); range->setEnabled(false); }
    interpolate_ = new QCheckBox("Interpolate", this); interpolate_->setChecked(true);
    for (const auto& name : colormapNames()) colormap_->addItem(QString::fromStdString(name));
    colormap_->addItem(kCategoricalColormap);
    levelLabel_ = new QLabel("Vertical level", this);
    form->addRow("Variable", variable_);
    form->addRow("Time step", time_); form->addRow("", play_);
    form->addRow(levelLabel_, level_);
    form->addRow("Colormap", colormap_); form->addRow("Units", units_);
    form->addRow("Opacity", opacity_);
    form->addRow("", autoRange_); form->addRow("Minimum", minimum_); form->addRow("Maximum", maximum_);
    form->addRow("", interpolate_);
    propertiesGroup_->setLayout(form);
    layout->addWidget(propertiesGroup_);

    colorbarGroup_ = new QGroupBox("Colorbar", this);
    auto* colorbarForm = new QFormLayout;
    tickCount_ = new QSpinBox(this); tickCount_->setRange(2, 11); tickCount_->setValue(3);
    tickFormat_ = new QComboBox(this);
    tickFormat_->addItem("Auto", "auto"); tickFormat_->addItem("Fixed", "fixed"); tickFormat_->addItem("Scientific", "scientific");
    tickDecimals_ = new QSpinBox(this); tickDecimals_->setRange(0, 10); tickDecimals_->setValue(2); tickDecimals_->setEnabled(false);
    colorbarForm->addRow("Tick count", tickCount_);
    colorbarForm->addRow("Tick format", tickFormat_);
    colorbarForm->addRow("Tick decimals", tickDecimals_);
    colorbarGroup_->setLayout(colorbarForm);
    layout->addWidget(colorbarGroup_);

    zoomGroup_ = new QGroupBox("View", this);
    auto* zoomLayout = new QVBoxLayout;
    auto* zoomButton = new QPushButton("Zoom to Layer", this);
    zoomLayout->addWidget(zoomButton);
    zoomGroup_->setLayout(zoomLayout);
    layout->addWidget(zoomGroup_);

    preview_ = new QLabel("Open a WRF/WPS NetCDF file, then Add Layer.", this);
    preview_->setMinimumSize(280, 120); preview_->setScaledContents(false); preview_->setWordWrap(true);
    status_ = new QLabel(this);
    layout->addWidget(preview_); layout->addWidget(status_); layout->addStretch();

    connect(openFileButton, &QPushButton::clicked, this, [this] { openFile(); });
    connect(closeFileButton_, &QPushButton::clicked, this, [this] { closeSelectedFile(); });
    connect(fileTree_, &QTreeWidget::currentItemChanged, this, [this] { closeFileButton_->setEnabled(fileTree_->currentItem() != nullptr); });
    connect(addLayerButton, &QPushButton::clicked, this, [this] { addLayer(); });
    connect(removeLayerButton_, &QPushButton::clicked, this, [this] { removeSelectedLayer(); });
    connect(moveUpButton_, &QPushButton::clicked, this, [this] { moveSelectedLayer(1); });
    connect(moveDownButton_, &QPushButton::clicked, this, [this] { moveSelectedLayer(-1); });
    connect(layerTree_, &QTreeWidget::currentItemChanged, this, [this] { onLayerSelectionChanged(); });
    connect(layerTree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) { onLayerCheckStateChanged(item); });
    connect(variable_, &QComboBox::currentIndexChanged, this, [this] { onVariableChanged(); });
    connect(time_, &QComboBox::currentIndexChanged, this, [this] { applyFieldsToSelectedLayer(); });
    connect(level_, &QSpinBox::valueChanged, this, [this] { applyFieldsToSelectedLayer(); });
    connect(colormap_, &QComboBox::currentIndexChanged, this, [this] { applyFieldsToSelectedLayer(); });
    connect(units_, &QComboBox::currentIndexChanged, this, [this] { applyFieldsToSelectedLayer(); });
    connect(opacity_, &QDoubleSpinBox::valueChanged, this, [this] { applyFieldsToSelectedLayer(); });
    connect(autoRange_, &QCheckBox::toggled, this, [this](bool checked) { minimum_->setEnabled(!checked); maximum_->setEnabled(!checked); applyFieldsToSelectedLayer(); });
    connect(minimum_, &QDoubleSpinBox::valueChanged, this, [this] { applyFieldsToSelectedLayer(); });
    connect(maximum_, &QDoubleSpinBox::valueChanged, this, [this] { applyFieldsToSelectedLayer(); });
    connect(interpolate_, &QCheckBox::toggled, this, [this] { applyFieldsToSelectedLayer(); });
    connect(tickCount_, &QSpinBox::valueChanged, this, [this] { onTickSettingsChanged(); });
    connect(tickFormat_, &QComboBox::currentIndexChanged, this, [this](int) { tickDecimals_->setEnabled(tickFormat_->currentData().toString() != "auto"); onTickSettingsChanged(); });
    connect(tickDecimals_, &QSpinBox::valueChanged, this, [this] { onTickSettingsChanged(); });
    connect(zoomButton, &QPushButton::clicked, this, [this] { zoomToSelectedLayer(); });
    playbackTimer_->setInterval(600);
    connect(play_, &QCheckBox::toggled, this, [this](bool enabled) { if (enabled) playbackTimer_->start(); else playbackTimer_->stop(); });
    connect(playbackTimer_, &QTimer::timeout, this, [this] { if (time_->count() > 0) time_->setCurrentIndex((time_->currentIndex() + 1) % time_->count()); });

    updatePanelVisibility();
}

void ViewForm::openFile() {
    const auto paths = QFileDialog::getOpenFileNames(this, "Open WRF/WPS NetCDF", {}, "NetCDF files (*.nc);;All files (*)");
    if (paths.isEmpty()) return;
    std::vector<std::string> given;
    for (const auto& path : paths) given.push_back(path.toStdString());
    openFiles(given);
}

void ViewForm::openFiles(const std::vector<std::string>& paths) {
    std::vector<std::filesystem::path> given(paths.begin(), paths.end());
    const auto grouped = groupWrfPaths(given);
    std::optional<std::string> lastOpened;
    try {
        for (const auto& group : grouped.groups) { static_cast<void>(renderer_.openFile(group)); lastOpened = group.front().string(); }
        for (const auto& single : grouped.singles) { static_cast<void>(renderer_.openFile({single})); lastOpened = single.string(); }
    } catch (const std::exception& error) { QMessageBox::critical(this, "Could not open file", error.what()); }
    rebuildFileList(lastOpened);
}

void ViewForm::closeSelectedFile() {
    const auto path = selectedFilePath();
    if (!path) return;
    const auto affected = static_cast<int>(std::count_if(layers_.begin(), layers_.end(), [&path](const ViewLayer& layer) { return layer.filePath == *path; }));
    if (affected > 0) {
        const auto question = QString("%1 layer(s) use this file. Closing it will remove them. Continue?").arg(affected);
        if (QMessageBox::question(this, "Close File", question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    }
    layers_.erase(std::remove_if(layers_.begin(), layers_.end(), [&path](const ViewLayer& layer) { return layer.filePath == *path; }), layers_.end());
    renderer_.invalidateFile(*path);
    rebuildFileList();
    rebuildLayerTree();
    refreshMap();
}

std::optional<std::string> ViewForm::selectedFilePath() const {
    if (!fileTree_->currentItem()) return std::nullopt;
    return fileTree_->currentItem()->data(0, kLayerIdRole).toString().toStdString();
}

std::vector<std::string> ViewForm::openFilePaths() const { return renderer_.openPaths(); }

void ViewForm::rebuildFileList(const std::optional<std::string>& selectPath) {
    fileTree_->blockSignals(true);
    fileTree_->clear();
    QTreeWidgetItem* toSelect = nullptr;
    for (const auto& path : openFilePaths()) {
        auto* item = new QTreeWidgetItem({QString::fromStdString(renderer_.openFile({path}).displayName())});
        item->setData(0, kLayerIdRole, QString::fromStdString(path));
        fileTree_->addTopLevelItem(item);
        if (selectPath && path == *selectPath) toSelect = item;
    }
    fileTree_->blockSignals(false);
    if (toSelect) fileTree_->setCurrentItem(toSelect);
    closeFileButton_->setEnabled(fileTree_->currentItem() != nullptr);
}

void ViewForm::addLayer() {
    const auto paths = openFilePaths();
    if (paths.empty()) return;
    const auto target = selectedFilePath().value_or(paths.front());
    auto& source = renderer_.openFile({target});
    if (source.variables().empty()) return;
    const auto& firstVariable = source.variables().front();
    ViewLayer layer{.layerId = nextLayerId_++, .filePath = target, .settings = {}};
    layer.settings.variable = firstVariable.name;
    if (firstVariable.categoryScheme) layer.settings.colormap = kCategoricalColormap;
    const bool wasEmpty = layers_.empty();
    layers_.push_back(layer);
    rebuildLayerTree(layer.layerId);
    refreshMap();
    if (wasEmpty) zoomToSelectedLayer();
}

void ViewForm::removeSelectedLayer() {
    if (!selectedLayerId_) return;
    layers_.erase(std::remove_if(layers_.begin(), layers_.end(), [this](const ViewLayer& layer) { return layer.layerId == *selectedLayerId_; }), layers_.end());
    rebuildLayerTree();
    refreshMap();
}

std::optional<std::size_t> ViewForm::selectedLayerIndex() const {
    if (!selectedLayerId_) return std::nullopt;
    for (std::size_t i = 0; i < layers_.size(); ++i) if (layers_[i].layerId == *selectedLayerId_) return i;
    return std::nullopt;
}

ViewLayer* ViewForm::selectedLayer() {
    const auto index = selectedLayerIndex();
    return index ? &layers_[*index] : nullptr;
}

void ViewForm::moveSelectedLayer(int direction) {
    const auto index = selectedLayerIndex();
    if (!index) return;
    const auto newIndex = static_cast<long>(*index) + direction;
    if (newIndex < 0 || newIndex >= static_cast<long>(layers_.size())) return;
    std::swap(layers_[*index], layers_[static_cast<std::size_t>(newIndex)]);
    rebuildLayerTree(selectedLayerId_);
    refreshMap();
}

std::string ViewForm::layerLabel(const ViewLayer& layer) {
    // renderer_.openFile() here is a cache lookup (the source is already open),
    // matching rasterlayer.RasterLayer.label()'s use of the renderer for
    // the display name, so a series shows its "(N files)" name.
    return layer.settings.variable + " — " + renderer_.openFile({layer.filePath}).displayName() + " (t=" + std::to_string(layer.settings.timeIndex + 1) + ")";
}

void ViewForm::rebuildLayerTree(std::optional<int> selectId) {
    updatingLayerTree_ = true;
    layerTree_->blockSignals(true);
    layerTree_->clear();
    // layers_ is bottom-first (draw order); display topmost-first so the
    // tree visually matches paint order top-to-bottom.
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        auto* item = new QTreeWidgetItem({QString::fromStdString(layerLabel(*it))});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, it->settings.visible ? Qt::Checked : Qt::Unchecked);
        item->setData(0, kLayerIdRole, it->layerId);
        layerTree_->addTopLevelItem(item);
    }
    layerTree_->blockSignals(false);
    updatingLayerTree_ = false;

    selectedLayerId_.reset();
    if (selectId) {
        for (int i = 0; i < layerTree_->topLevelItemCount(); ++i) {
            auto* item = layerTree_->topLevelItem(i);
            if (item->data(0, kLayerIdRole).toInt() == *selectId) { layerTree_->setCurrentItem(item); selectedLayerId_ = *selectId; break; }
        }
    }
    if (!selectedLayerId_) play_->setChecked(false);
    updatePanelVisibility();
    populatePropertiesPanel();
}

void ViewForm::onLayerSelectionChanged() {
    selectedLayerId_ = layerTree_->currentItem() ? std::optional<int>(layerTree_->currentItem()->data(0, kLayerIdRole).toInt()) : std::nullopt;
    play_->setChecked(false);
    updatePanelVisibility();
    populatePropertiesPanel();
    updateColorbar();
}

void ViewForm::onLayerCheckStateChanged(QTreeWidgetItem* item) {
    if (updatingLayerTree_) return;
    const int id = item->data(0, kLayerIdRole).toInt();
    for (auto& layer : layers_) if (layer.layerId == id) layer.settings.visible = item->checkState(0) == Qt::Checked;
    refreshMap();
}

void ViewForm::updatePanelVisibility() {
    const bool hasSelection = selectedLayerId_.has_value();
    propertiesGroup_->setVisible(hasSelection);
    colorbarGroup_->setVisible(hasSelection);
    zoomGroup_->setVisible(hasSelection);
    removeLayerButton_->setEnabled(hasSelection);
    const auto index = selectedLayerIndex();
    moveUpButton_->setEnabled(hasSelection && index && *index + 1 < layers_.size());
    moveDownButton_->setEnabled(hasSelection && index && *index > 0);
}

void ViewForm::populatePropertiesPanel() {
    auto* layer = selectedLayer();
    if (!layer) return;
    try {
        auto& source = renderer_.openFile({layer->filePath});
        const auto old = variable_->blockSignals(true);
        variable_->clear();
        for (const auto& value : source.variables()) variable_->addItem(QString::fromStdString(value.name));
        variable_->setCurrentIndex(std::max(0, variable_->findText(QString::fromStdString(layer->settings.variable))));
        variable_->blockSignals(old);

        const auto found = std::find_if(source.variables().begin(), source.variables().end(), [layer](const WrfVariable& v) { return v.name == layer->settings.variable; });
        if (found == source.variables().end()) return;

        const auto* seriesTimes = source.seriesTimes();
        const int timeCount = seriesTimes ? static_cast<int>(seriesTimes->size()) : found->timeCount;
        const auto timeOld = time_->blockSignals(true);
        time_->clear();
        for (int i = 0; i < timeCount; ++i)
            time_->addItem(seriesTimes ? QString::fromStdString(seriesTimes->at(static_cast<std::size_t>(i))) : QString("Step %1 of %2").arg(i + 1).arg(timeCount));
        time_->setCurrentIndex(std::clamp(layer->settings.timeIndex, 0, std::max(0, timeCount - 1)));
        time_->blockSignals(timeOld);
        play_->setEnabled(timeCount > 1);

        levelLabel_->setVisible(found->extraDimension.has_value());
        level_->setVisible(found->extraDimension.has_value());
        const auto levelOld = level_->blockSignals(true);
        level_->setMaximum(std::max(1, found->levelCount));
        level_->setValue(layer->settings.levelIndex + 1);
        level_->blockSignals(levelOld);

        const auto unitsOld = units_->blockSignals(true);
        units_->clear();
        for (const auto& unit : conversionsFor(found->units)) units_->addItem(QString::fromStdString(unit.label), QString::fromStdString(unit.key));
        const auto unitIndex = units_->findData(QString::fromStdString(layer->settings.unitKey));
        units_->setCurrentIndex(std::max(0, unitIndex));
        units_->setVisible(units_->count() > 1);
        units_->blockSignals(unitsOld);

        const auto colormapOld = colormap_->blockSignals(true);
        colormap_->setCurrentText(QString::fromStdString(layer->settings.colormap));
        colormap_->blockSignals(colormapOld);

        opacity_->blockSignals(true); opacity_->setValue(layer->settings.opacity); opacity_->blockSignals(false);
        const bool isAuto = !layer->settings.minimum && !layer->settings.maximum;
        autoRange_->blockSignals(true); autoRange_->setChecked(isAuto); autoRange_->blockSignals(false);
        minimum_->setEnabled(!isAuto); maximum_->setEnabled(!isAuto);
        if (!isAuto) {
            minimum_->blockSignals(true); minimum_->setValue(layer->settings.minimum.value_or(0)); minimum_->blockSignals(false);
            maximum_->blockSignals(true); maximum_->setValue(layer->settings.maximum.value_or(0)); maximum_->blockSignals(false);
        }
        interpolate_->blockSignals(true); interpolate_->setChecked(layer->settings.interpolate); interpolate_->blockSignals(false);

        tickCount_->blockSignals(true); tickCount_->setValue(layer->settings.tickCount); tickCount_->blockSignals(false);
        const auto tickFormatOld = tickFormat_->blockSignals(true);
        tickFormat_->setCurrentIndex(std::max(0, tickFormat_->findData(QString::fromStdString(layer->settings.tickFormat))));
        tickFormat_->blockSignals(tickFormatOld);
        tickDecimals_->blockSignals(true); tickDecimals_->setValue(layer->settings.tickDecimals); tickDecimals_->blockSignals(false);
        tickDecimals_->setEnabled(layer->settings.tickFormat != "auto");
    } catch (const std::exception& error) { status_->setText(error.what()); }
}

void ViewForm::onVariableChanged() {
    auto* layer = selectedLayer();
    if (!layer) return;
    layer->settings.variable = variable_->currentText().toStdString();
    layer->settings.levelIndex = 0;
    layer->settings.unitKey = "native";
    try {
        auto& source = renderer_.openFile({layer->filePath});
        const auto found = std::find_if(source.variables().begin(), source.variables().end(), [layer](const WrfVariable& v) { return v.name == layer->settings.variable; });
        if (found != source.variables().end()) {
            if (found->categoryScheme) layer->settings.colormap = kCategoricalColormap;
            else if (layer->settings.colormap == kCategoricalColormap) layer->settings.colormap = "viridis";
        }
    } catch (const std::exception&) {}
    rebuildLayerTree(selectedLayerId_);
    refreshMap();
}

void ViewForm::applyFieldsToSelectedLayer() {
    auto* layer = selectedLayer();
    if (!layer) return;
    layer->settings.timeIndex = std::max(0, time_->currentIndex());
    layer->settings.levelIndex = std::max(0, level_->value() - 1);
    layer->settings.colormap = colormap_->currentText().toStdString();
    layer->settings.unitKey = units_->currentData().toString().toStdString();
    layer->settings.opacity = opacity_->value();
    layer->settings.interpolate = interpolate_->isChecked();
    if (autoRange_->isChecked()) { layer->settings.minimum.reset(); layer->settings.maximum.reset(); }
    else { layer->settings.minimum = static_cast<float>(minimum_->value()); layer->settings.maximum = static_cast<float>(maximum_->value()); }
    refreshMap();
}

void ViewForm::onTickSettingsChanged() {
    auto* layer = selectedLayer();
    if (!layer) return;
    layer->settings.tickCount = tickCount_->value();
    layer->settings.tickFormat = tickFormat_->currentData().toString().toStdString();
    layer->settings.tickDecimals = tickDecimals_->value();
    // Cosmetic legend-only change - tick fields are excluded from
    // LayerRenderer's cache keys, so this must not re-render/invalidate
    // anything; just rebuild the colorbar pixmap directly.
    updateColorbar();
}

void ViewForm::refreshMap() {
    std::vector<RasterOverlay> overlays;
    for (auto& layer : layers_) {  // bottom-first: draw order
        if (!layer.settings.visible) continue;
        try {
            const auto rendered = renderer_.render(layer.filePath, layer.settings);
            overlays.push_back({rasterImage(rendered), rendered.bounds3857, layer.settings.opacity, layer.settings.interpolate});
        } catch (const UserError&) {
            // A lazily-detected series mismatch or a bad time/level index:
            // drop this layer's overlay rather than failing the whole redraw.
        }
    }
    map_->setRasterOverlayGroup("view-rasters", overlays);
    if (!overlays.empty()) preview_->setPixmap(QPixmap::fromImage(overlays.back().image).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    else preview_->setText("Open a WRF/WPS NetCDF file, then Add Layer.");
    updateColorbar();
}

void ViewForm::updateColorbar() {
    auto* layer = selectedLayer();
    if (!layer || !layer->settings.visible) { map_->setLegend({}); map_->setInfoText({}); return; }
    try {
        auto& source = renderer_.openFile({layer->filePath});
        const auto rendered = renderer_.render(layer->filePath, layer->settings);
        const auto found = std::find_if(source.variables().begin(), source.variables().end(), [layer](const WrfVariable& v) { return v.name == layer->settings.variable; });
        const auto unitLabel = found != source.variables().end() ? findUnit(found->units, layer->settings.unitKey).label : std::string{};
        const auto title = layer->settings.variable + (unitLabel.empty() ? "" : " (" + unitLabel + ")");
        if (layer->settings.colormap == kCategoricalColormap)
            map_->setLegend(buildCategoricalLegend(rendered.categoricalPalette, rendered.categoricalLabels, rendered.presentCategories, title));
        else
            map_->setLegend(buildColorbar(title, rendered.minimum, rendered.maximum, colormap(layer->settings.colormap),
                layer->settings.tickCount, layer->settings.tickFormat, layer->settings.tickDecimals));
        const auto timeLabel = time_->currentIndex() >= 0 ? time_->currentText().toStdString() : std::string{};
        map_->setInfoText(QString::fromStdString(title + "  —  " + timeLabel));
    } catch (const std::exception&) { map_->setLegend({}); map_->setInfoText({}); }
}

void ViewForm::zoomToSelectedLayer() {
    auto* layer = selectedLayer();
    if (!layer) return;
    try {
        const auto rendered = renderer_.render(layer->filePath, layer->settings);
        const auto southWest = mercatorToLonLat(rendered.bounds3857.minX, rendered.bounds3857.minY);
        const auto northEast = mercatorToLonLat(rendered.bounds3857.maxX, rendered.bounds3857.maxY);
        map_->zoomToBounds(southWest, northEast);
        hasAutoZoomed_ = true;
    } catch (const std::exception& error) { status_->setText(error.what()); }
}
}  // namespace wrftools
