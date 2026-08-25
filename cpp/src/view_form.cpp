#include "wrftools/view_form.hpp"

#include "wrftools/colormaps.hpp"
#include "wrftools/colorbar.hpp"
#include "wrftools/raster_layer.hpp"
#include "wrftools/units.hpp"
#include "wrftools/wrf_file.hpp"
#include "wrftools/tile_map_widget.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace wrftools {
ViewForm::ViewForm(TileMapWidget* map, QWidget* parent) : QWidget(parent), map_(map) {
    auto* layout = new QVBoxLayout(this);
    auto* open = new QPushButton("Open WRF/WPS NetCDF…", this);
    layout->addWidget(open);
    auto* form = new QFormLayout;
    variable_ = new QComboBox(this); colormap_ = new QComboBox(this); units_ = new QComboBox(this);
    time_ = new QSpinBox(this); level_ = new QSpinBox(this);
    time_->setMinimum(1); level_->setMinimum(1);
    for (const auto& name : colormapNames()) colormap_->addItem(QString::fromStdString(name));
    form->addRow("Variable", variable_); form->addRow("Time step", time_); form->addRow("Vertical level", level_); form->addRow("Colormap", colormap_); form->addRow("Units", units_);
    layout->addLayout(form);
    preview_ = new QLabel("Open a WRF/WPS NetCDF file to add a native raster layer.", this);
    preview_->setMinimumSize(280, 180); preview_->setScaledContents(false); preview_->setWordWrap(true);
    status_ = new QLabel(this); layout->addWidget(preview_); layout->addWidget(status_); layout->addStretch();
    connect(open, &QPushButton::clicked, this, [this] { openFile(); });
    connect(variable_, &QComboBox::currentIndexChanged, this, [this] { refreshVariables(); renderSelected(); });
    connect(colormap_, &QComboBox::currentIndexChanged, this, [this] { renderSelected(); });
    connect(units_, &QComboBox::currentIndexChanged, this, [this] { renderSelected(); });
    connect(time_, &QSpinBox::valueChanged, this, [this] { renderSelected(); });
    connect(level_, &QSpinBox::valueChanged, this, [this] { renderSelected(); });
}
void ViewForm::openFile() {
    const auto path = QFileDialog::getOpenFileName(this, "Open WRF/WPS NetCDF", {}, "NetCDF files (*.nc);;All files (*)");
    if (path.isEmpty()) return;
    try { file_ = std::make_unique<WrfFile>(path.toStdString()); refreshVariables(); renderSelected(); }
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
    time_->setMaximum(selected.timeCount); level_->setMaximum(selected.levelCount);
    units_->clear(); for (const auto& unit : conversionsFor(selected.units)) units_->addItem(QString::fromStdString(unit.label), QString::fromStdString(unit.key));
}
void ViewForm::renderSelected() {
    if (!file_ || variable_->currentIndex() < 0) return;
    try {
        RasterLayer layer{.variable = variable_->currentText().toStdString(), .timeIndex = time_->value() - 1, .levelIndex = level_->value() - 1, .colormap = colormap_->currentText().toStdString(), .minimum = std::nullopt, .maximum = std::nullopt, .unitKey = units_->currentData().toString().toStdString()};
        const auto rendered = renderLayer(*file_, layer);
        const auto image = rasterImage(rendered);
        const auto& bounds = file_->geographicBounds();
        map_->setRasterOverlays({{image, {bounds.west, bounds.south}, {bounds.east, bounds.north}, layer.opacity, layer.interpolate}});
        const auto& selected = file_->variables().at(static_cast<std::size_t>(variable_->currentIndex()));
        const auto unit = findUnit(selected.units, layer.unitKey);
        map_->setLegend(buildColorbar(layer.variable + " (" + unit.label + ")", rendered.minimum, rendered.maximum, colormap(layer.colormap)));
        preview_->setPixmap(QPixmap::fromImage(image).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
        status_->setText(QString("%1 × %2   range %3 … %4").arg(rendered.width).arg(rendered.height).arg(rendered.minimum).arg(rendered.maximum));
    } catch (const std::exception& error) { status_->setText(error.what()); }
}
}  // namespace wrftools
