#include "wrftools/view_form.hpp"

#include "wrftools/colormaps.hpp"
#include "wrftools/colorbar.hpp"
#include "wrftools/raster_layer.hpp"
#include "wrftools/units.hpp"
#include "wrftools/wrf_file.hpp"
#include "wrftools/tile_map_widget.hpp"

#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace wrftools {
ViewForm::ViewForm(TileMapWidget* map, QWidget* parent) : QWidget(parent), map_(map) {
    auto* layout = new QVBoxLayout(this);
    auto* open = new QPushButton("Open WRF/WPS NetCDF…", this);
    auto* close = new QPushButton("Close File", this);
    layout->addWidget(open);
    layout->addWidget(close);
    auto* form = new QFormLayout;
    variable_ = new QComboBox(this); colormap_ = new QComboBox(this); units_ = new QComboBox(this);
    time_ = new QSpinBox(this); level_ = new QSpinBox(this); play_ = new QCheckBox("Play", this); visible_ = new QCheckBox("Visible", this); visible_->setChecked(true); playbackTimer_ = new QTimer(this);
    opacity_ = new QDoubleSpinBox(this); minimum_ = new QDoubleSpinBox(this); maximum_ = new QDoubleSpinBox(this);
    time_->setMinimum(1); level_->setMinimum(1);
    opacity_->setRange(0.0, 1.0); opacity_->setSingleStep(0.1); opacity_->setValue(0.8);
    for (auto* range : {minimum_, maximum_}) { range->setRange(-1e30, 1e30); range->setDecimals(4); range->setSpecialValueText("Auto"); range->setValue(-1e30); }
    for (const auto& name : colormapNames()) colormap_->addItem(QString::fromStdString(name));
    colormap_->addItem(kCategoricalColormap);
    form->addRow("Variable", variable_); form->addRow("Time step", time_); form->addRow("", play_); form->addRow("", visible_); form->addRow("Vertical level", level_); form->addRow("Colormap", colormap_); form->addRow("Units", units_); form->addRow("Opacity", opacity_); form->addRow("Minimum", minimum_); form->addRow("Maximum", maximum_);
    layout->addLayout(form);
    preview_ = new QLabel("Open a WRF/WPS NetCDF file to add a native raster layer.", this);
    preview_->setMinimumSize(280, 180); preview_->setScaledContents(false); preview_->setWordWrap(true);
    status_ = new QLabel(this); layout->addWidget(preview_); layout->addWidget(status_); layout->addStretch();
    connect(open, &QPushButton::clicked, this, [this] { openFile(); });
    connect(close, &QPushButton::clicked, this, [this] { closeFile(); });
    connect(variable_, &QComboBox::currentIndexChanged, this, [this] { refreshVariables(); renderSelected(); });
    connect(colormap_, &QComboBox::currentIndexChanged, this, [this] { renderSelected(); });
    connect(units_, &QComboBox::currentIndexChanged, this, [this] { renderSelected(); });
    connect(time_, &QSpinBox::valueChanged, this, [this] { renderSelected(); });
    connect(level_, &QSpinBox::valueChanged, this, [this] { renderSelected(); });
    connect(visible_, &QCheckBox::toggled, this, [this] { renderSelected(); });
    connect(opacity_, &QDoubleSpinBox::valueChanged, this, [this] { renderSelected(); });
    connect(minimum_, &QDoubleSpinBox::valueChanged, this, [this] { renderSelected(); });
    connect(maximum_, &QDoubleSpinBox::valueChanged, this, [this] { renderSelected(); });
    playbackTimer_->setInterval(500);
    connect(play_, &QCheckBox::toggled, this, [this](bool enabled) { if (enabled) playbackTimer_->start(); else playbackTimer_->stop(); });
    connect(playbackTimer_, &QTimer::timeout, this, [this] { time_->setValue(time_->value() == time_->maximum() ? time_->minimum() : time_->value() + 1); });
}
void ViewForm::closeFile() {
    playbackTimer_->stop(); play_->setChecked(false); file_.reset(); variable_->clear(); units_->clear();
    preview_->setText("Open a WRF/WPS NetCDF file to add a native raster layer.");
    status_->clear(); map_->clearRasterOverlayGroup("view-rasters"); map_->setLegend({}); hasAutoZoomed_ = false;
}
void ViewForm::openFile() {
    const auto path = QFileDialog::getOpenFileName(this, "Open WRF/WPS NetCDF", {}, "NetCDF files (*.nc);;All files (*)");
    if (path.isEmpty()) return;
    try { file_ = std::make_unique<WrfFile>(path.toStdString()); hasAutoZoomed_ = false; refreshVariables(); renderSelected(); }
    catch (const std::exception& error) { QMessageBox::critical(this, "Could not open file", error.what()); }
}
void ViewForm::refreshVariables() {
    if (!file_) return;
    const auto old = variable_->blockSignals(true); variable_->clear();
    for (const auto& value : file_->variables()) variable_->addItem(QString::fromStdString(value.name));
    variable_->blockSignals(old);
    const auto index = variable_->currentIndex();
    if (index < 0) return;
    const auto& selected = file_->variables().at(static_cast<std::size_t>(index));
    if (selected.categoryScheme) colormap_->setCurrentText(kCategoricalColormap);
    time_->setMaximum(selected.timeCount); level_->setMaximum(selected.levelCount);
    play_->setEnabled(selected.timeCount > 1);
    if (selected.timeCount <= 1) play_->setChecked(false);
    units_->clear(); for (const auto& unit : conversionsFor(selected.units)) units_->addItem(QString::fromStdString(unit.label), QString::fromStdString(unit.key));
}
void ViewForm::renderSelected() {
    if (!file_ || variable_->currentIndex() < 0) return;
    if (!visible_->isChecked()) { map_->clearRasterOverlayGroup("view-rasters"); map_->setLegend({}); preview_->clear(); status_->setText("Layer hidden"); return; }
    try {
        const auto autoValue = -1e30;
        RasterLayer layer{.variable = variable_->currentText().toStdString(), .timeIndex = time_->value() - 1, .levelIndex = level_->value() - 1, .colormap = colormap_->currentText().toStdString(), .minimum = minimum_->value() == autoValue ? std::nullopt : std::optional<float>(minimum_->value()), .maximum = maximum_->value() == autoValue ? std::nullopt : std::optional<float>(maximum_->value()), .unitKey = units_->currentData().toString().toStdString(), .opacity = opacity_->value()};
        const auto rendered = renderLayer(*file_, layer);
        const auto image = rasterImage(rendered);
        map_->setRasterOverlayGroup("view-rasters", {{image, rendered.bounds3857, layer.opacity, layer.interpolate}});
        if (!hasAutoZoomed_) { const auto& bounds = file_->geographicBounds(); map_->zoomToBounds({bounds.west, bounds.south}, {bounds.east, bounds.north}); hasAutoZoomed_ = true; }
        const auto& selected = file_->variables().at(static_cast<std::size_t>(variable_->currentIndex()));
        const auto unit = findUnit(selected.units, layer.unitKey);
        const auto title = layer.variable + " (" + unit.label + ")";
        if (layer.colormap == kCategoricalColormap) map_->setLegend(buildCategoricalLegend(rendered.categoricalPalette, rendered.categoricalLabels, rendered.presentCategories, title));
        else map_->setLegend(buildColorbar(title, rendered.minimum, rendered.maximum, colormap(layer.colormap)));
        preview_->setPixmap(QPixmap::fromImage(image).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
        status_->setText(QString("%1 × %2   range %3 … %4").arg(rendered.width).arg(rendered.height).arg(rendered.minimum).arg(rendered.maximum));
    } catch (const std::exception& error) { status_->setText(error.what()); }
}
}  // namespace wrftools
