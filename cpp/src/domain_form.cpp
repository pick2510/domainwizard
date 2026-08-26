#include "wrftools/domain_form.hpp"
#include "wrftools/domain_overlay.hpp"
#include "wrftools/error.hpp"
#include "wrftools/file_extent.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/wps_namelist.hpp"

#include <QComboBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <map>

namespace wrftools {
namespace {
constexpr int kDomainIdRole = Qt::UserRole;
constexpr int kMaxReasonableDimension = 5000;

// Green = valid, yellow = empty-but-required, red = non-empty-but-invalid,
// no styling = empty-and-not-required. Mirrors formhelpers.py's
// update_input_validation_style, applied on every editingFinished.
void styleValidation(QLineEdit* field, bool required) {
    const bool empty = field->text().trimmed().isEmpty();
    if (empty) { field->setStyleSheet(required ? "QLineEdit { background-color: #fff79a }" : ""); return; }
    int pos = 0; QString text = field->text();
    const bool valid = field->validator() && field->validator()->validate(text, pos) == QValidator::Acceptable;
    field->setStyleSheet(valid ? "QLineEdit { background-color: #c4df9b }" : "QLineEdit { background-color: #f6989d }");
}

bool isValid(const QLineEdit* field) {
    if (field->text().trimmed().isEmpty()) return false;
    int pos = 0; QString text = field->text();
    return field->validator() && field->validator()->validate(text, pos) == QValidator::Acceptable;
}

QLineEdit* makeField(QFormLayout* form, const QString& label, QValidator* validator) {
    auto* field = new QLineEdit;
    field->setValidator(validator);
    QObject::connect(field, &QLineEdit::textChanged, field, [field, validator] { styleValidation(field, validator != nullptr); });
    form->addRow(label, field);
    return field;
}
}  // namespace

DomainForm::DomainForm(TileMapWidget* map, QWidget* parent) : QWidget(parent), map_(map) {
    auto* layout = new QVBoxLayout(this);
    auto* import = new QPushButton("Import namelist.wps…", this);
    auto* exportFile = new QPushButton("Export namelist.wps…", this);
    layout->addWidget(import); layout->addWidget(exportFile);

    tree_ = new QTreeWidget(this); tree_->setHeaderHidden(true);
    auto* add = new QPushButton("Add Child Domain", this);
    auto* remove = new QPushButton("Remove Domain", this);
    layout->addWidget(tree_); layout->addWidget(add); layout->addWidget(remove);

    mapTypeGroup_ = new QGroupBox("Map Type", this);
    auto* mapTypeForm = new QFormLayout;
    mapTypeForm->setVerticalSpacing(10);
    projection_ = new QComboBox(this);
    projection_->setMinimumHeight(28);
    projection_->addItem("Latitude/Longitude", "lat-lon");
    projection_->addItem("Lambert Conformal", "lambert");
    projection_->addItem("Mercator", "mercator");
    projection_->addItem("Polar Stereographic", "polar");
    mapTypeForm->addRow("Projection", projection_);
    trueLat1_ = makeField(mapTypeForm, "Truelat 1", new QDoubleValidator(-90, 90, 10, this));
    trueLat2_ = makeField(mapTypeForm, "Truelat 2", new QDoubleValidator(-90, 90, 10, this));
    standLon_ = makeField(mapTypeForm, "Standard Longitude", new QDoubleValidator(-180, 180, 10, this));
    mapTypeGroup_->setLayout(mapTypeForm);
    layout->addWidget(mapTypeGroup_);

    resolutionGroup_ = new QGroupBox("Horizontal Grid Spacing", this);
    auto* resolutionForm = new QFormLayout;
    resolutionForm->setVerticalSpacing(10);
    resolution_ = makeField(resolutionForm, "Resolution", new QDoubleValidator(1e-20, 1e12, 20, this));
    resolutionGroup_->setLayout(resolutionForm);
    layout->addWidget(resolutionGroup_);

    nestingGroup_ = new QGroupBox("Nesting", this);
    auto* nestingForm = new QFormLayout;
    nestingForm->setVerticalSpacing(10);
    auto* ratioValidator = new QIntValidator(1, 100, this);
    ratio_ = makeField(nestingForm, "Child-to-Parent Ratio", ratioValidator);
    nestingGroup_->setLayout(nestingForm);
    layout->addWidget(nestingGroup_);

    extentCalcGroup_ = new QGroupBox("Grid Extent Calculator", this);
    auto* extentLayout = new QVBoxLayout;
    auto* setFromMap = new QPushButton("Set to Map View Extent", this);
    auto* setFromFile = new QPushButton("Set from File…", this);
    extentLayout->addWidget(setFromMap); extentLayout->addWidget(setFromFile);
    extentCalcGroup_->setLayout(extentLayout);
    layout->addWidget(extentCalcGroup_);

    centerGroup_ = new QGroupBox("Center Point", this);
    auto* centerForm = new QFormLayout;
    centerForm->setVerticalSpacing(10);
    centerLon_ = makeField(centerForm, "Longitude", new QDoubleValidator(-180, 180, 10, this));
    centerLat_ = makeField(centerForm, "Latitude", new QDoubleValidator(-90, 90, 10, this));
    centerGroup_->setLayout(centerForm);
    layout->addWidget(centerGroup_);

    positionGroup_ = new QGroupBox("Position within Parent", this);
    auto* positionForm = new QFormLayout;
    positionForm->setVerticalSpacing(10);
    auto* nonNegative = new QIntValidator(0, 1'000'000, this);
    paddingLeft_ = makeField(positionForm, "From left edge (parent cells)", nonNegative);
    paddingBottom_ = makeField(positionForm, "From bottom edge (parent cells)", new QIntValidator(0, 1'000'000, this));
    positionGroup_->setLayout(positionForm);
    layout->addWidget(positionGroup_);

    gridExtentGroup_ = new QGroupBox("Grid Extent", this);
    auto* gridForm = new QFormLayout;
    gridForm->setVerticalSpacing(10);
    columns_ = makeField(gridForm, "Horizontal (cells)", new QIntValidator(1, 1'000'000, this));
    rows_ = makeField(gridForm, "Vertical (cells)", new QIntValidator(1, 1'000'000, this));
    gridExtentGroup_->setLayout(gridForm);
    layout->addWidget(gridExtentGroup_);
    layout->addStretch(1);

    connect(import, &QPushButton::clicked, this, [this] { importNamelist(); });
    connect(exportFile, &QPushButton::clicked, this, [this] { exportNamelist(); });
    connect(add, &QPushButton::clicked, this, [this] { addChild(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { updateSelection(); });
    connect(projection_, &QComboBox::currentIndexChanged, this, [this] { updateProjectionParamVisibility(projection_->currentData().toString()); applySelectedDomainFields(false); });
    for (auto* field : {trueLat1_, trueLat2_, standLon_, resolution_, centerLon_, centerLat_, ratio_, paddingLeft_, paddingBottom_, columns_, rows_})
        connect(field, &QLineEdit::editingFinished, this, [this] { applySelectedDomainFields(false); });
    connect(setFromMap, &QPushButton::clicked, this, [this] { onSetMapExtentClicked(); });
    connect(setFromFile, &QPushButton::clicked, this, [this] { onSetFileExtentClicked(); });

    updatePanelVisibility();
}

void DomainForm::setActive(bool active) { active_ = active; }

void DomainForm::setProject(WpsProject project) { project_ = std::move(project); rebuildTree(); }

std::optional<int> DomainForm::selectedDomainId() const {
    if (!tree_->currentItem()) return std::nullopt;
    return tree_->currentItem()->data(0, kDomainIdRole).toInt();
}

void DomainForm::updateProjectionParamVisibility(const QString& projectionId) {
    trueLat1_->setEnabled(projectionId == "lambert" || projectionId == "mercator" || projectionId == "polar");
    trueLat2_->setEnabled(projectionId == "lambert");
    standLon_->setEnabled(projectionId == "lambert" || projectionId == "polar");
}

void DomainForm::updatePanelVisibility() {
    const auto id = selectedDomainId();
    const bool hasSelection = id.has_value();
    const bool isRoot = hasSelection && *id == 1;
    mapTypeGroup_->setVisible(isRoot);
    resolutionGroup_->setVisible(isRoot);
    centerGroup_->setVisible(isRoot);
    nestingGroup_->setVisible(hasSelection && !isRoot);
    positionGroup_->setVisible(hasSelection && !isRoot);
    extentCalcGroup_->setVisible(hasSelection);
    gridExtentGroup_->setVisible(hasSelection);
}

void DomainForm::populatePropertiesPanel() {
    const auto id = selectedDomainId();
    if (!project_ || !id) return;
    const auto& domain = project_->domains.domains().at(static_cast<std::size_t>(*id - 1));
    if (*id == 1) {
        const auto index = projection_->findData(QString::fromStdString(domain.mapProj));
        projection_->blockSignals(true); projection_->setCurrentIndex(std::max(0, index)); projection_->blockSignals(false);
        updateProjectionParamVisibility(QString::fromStdString(domain.mapProj));
        trueLat1_->setText(QString::number(domain.trueLat1));
        trueLat2_->setText(QString::number(domain.trueLat2));
        standLon_->setText(QString::number(domain.standLon));
        resolution_->setText(QString::number(domain.dx));
        centerLon_->setText(QString::number(domain.centerLon));
        centerLat_->setText(QString::number(domain.centerLat));
    } else {
        ratio_->setText(QString::number(domain.ratio));
        paddingLeft_->setText(QString::number(domain.paddingLeft));
        paddingBottom_->setText(QString::number(domain.paddingBottom));
    }
    columns_->setText(QString::number(domain.columns));
    rows_->setText(QString::number(domain.rows));
}

void DomainForm::updateSelection() {
    // Populate before revealing: mapTypeGroup_ (and the other per-selection
    // groups) start out hidden and only become visible here, the first time
    // a given selection needs them - if a group's fields are shown with
    // their still-default construction-time text (e.g. Projection's
    // "Latitude/Longitude") and only get their real values a moment later,
    // Qt's very first paint of a just-shown QFormLayout row can end up
    // laid out for the old text and then draw the new, differently-sized
    // text into that stale geometry, clipping it. Setting the final values
    // first means the first-ever show already lays out for real content.
    populatePropertiesPanel();
    updatePanelVisibility();
}

bool DomainForm::applySelectedDomainFields(bool raiseOnInvalid) {
    const auto id = selectedDomainId();
    if (!project_ || !id) return false;
    auto& domain = project_->domains.domains().at(static_cast<std::size_t>(*id - 1));
    bool ok = true;
    if (*id == 1) {
        const auto projectionId = projection_->currentData().toString().toStdString();
        std::vector<QLineEdit*> required{resolution_, centerLon_, centerLat_, columns_, rows_};
        if (projectionId == "lambert" || projectionId == "mercator" || projectionId == "polar") required.push_back(trueLat1_);
        if (projectionId == "lambert") required.push_back(trueLat2_);
        if (projectionId == "lambert" || projectionId == "polar") required.push_back(standLon_);
        for (auto* field : required) ok = ok && isValid(field);
        if (ok) {
            domain.mapProj = projectionId;
            domain.trueLat1 = (projectionId == "lat-lon") ? 0.0 : trueLat1_->text().toDouble();
            domain.trueLat2 = (projectionId == "lambert") ? trueLat2_->text().toDouble() : domain.trueLat1;
            domain.standLon = (projectionId == "lambert" || projectionId == "polar") ? standLon_->text().toDouble() : 0.0;
            domain.dx = domain.dy = resolution_->text().toDouble();
            domain.centerLon = centerLon_->text().toDouble();
            domain.centerLat = centerLat_->text().toDouble();
            domain.columns = columns_->text().toInt();
            domain.rows = rows_->text().toInt();
        }
    } else {
        for (auto* field : {ratio_, paddingLeft_, paddingBottom_, columns_, rows_}) ok = ok && isValid(field);
        if (ok) {
            domain.ratio = ratio_->text().toInt();
            domain.paddingLeft = paddingLeft_->text().toInt();
            domain.paddingBottom = paddingBottom_->text().toInt();
            domain.columns = columns_->text().toInt();
            domain.rows = rows_->text().toInt();
        }
    }
    if (!ok) {
        if (raiseOnInvalid) throw UserError("Domain configuration invalid or incomplete - check the highlighted fields (red = invalid, yellow = required but empty).");
        return false;
    }
    try {
        project_->domains.fillDomains();
    } catch (const UserError&) {
        if (raiseOnInvalid) throw;
        redraw(false);
        return false;
    }
    populatePropertiesPanel();
    redraw(false);
    return true;
}

void DomainForm::addChild() {
    if (!project_ || !tree_->currentItem()) return;
    const int parentId = *selectedDomainId();
    auto& domains = project_->domains.domains();
    const auto& parent = domains.at(static_cast<std::size_t>(parentId - 1));
    domains.push_back({.id = static_cast<int>(domains.size()) + 1, .parentId = parentId, .ratio = 3, .paddingLeft = 0, .paddingBottom = 0, .columns = std::max(1, parent.columns / 3), .rows = std::max(1, parent.rows / 3), .bounds = std::nullopt});
    rebuildTree();
}
void DomainForm::removeSelected() {
    const auto id = selectedDomainId();
    if (!project_ || !id) return;
    if (*id == 1) { QMessageBox::information(this, "Cannot remove domain", "The root domain cannot be removed."); return; }
    project_->domains.removeSubtree(*id);
    rebuildTree();
}
void DomainForm::importNamelist() {
    const auto path = QFileDialog::getOpenFileName(this, "Import WPS namelist", {}, "WPS namelist (*)");
    if (path.isEmpty()) return;
    try { setProject(readWpsNamelist(path.toStdString())); }
    catch (const std::exception& error) { QMessageBox::critical(this, "Could not import namelist", error.what()); }
}
void DomainForm::exportNamelist() {
    if (!project_) return;
    const auto path = QFileDialog::getSaveFileName(this, "Export WPS namelist", "namelist.wps", "WPS namelist (*)");
    if (path.isEmpty()) return;
    try { writeWpsNamelist(*project_, path.toStdString()); }
    catch (const std::exception& error) { QMessageBox::critical(this, "Could not export namelist", error.what()); }
}
void DomainForm::rebuildTree() {
    tree_->blockSignals(true);
    tree_->clear();
    if (project_) {
        std::map<int, QTreeWidgetItem*> items;
        for (const auto& domain : project_->domains.domains()) {
            auto* item = new QTreeWidgetItem({QString("Domain %1").arg(domain.id)});
            item->setData(0, kDomainIdRole, domain.id);
            items.emplace(domain.id, item);
            if (domain.id == 1) tree_->addTopLevelItem(item); else items.at(domain.parentId)->addChild(item);
        }
        tree_->expandAll();
    }
    tree_->blockSignals(false);
    if (tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
    updateSelection();
    redraw(true);
}

void DomainForm::onSetMapExtentClicked() {
    const auto [southWest, northEast] = map_->currentViewBounds();
    try { setDomainToExtent(Crs::wgs84(), {southWest.lon, southWest.lat, northEast.lon, northEast.lat}); }
    catch (const std::exception& error) { QMessageBox::critical(this, "Could not set extent", error.what()); }
}
void DomainForm::onSetFileExtentClicked() {
    const auto path = QFileDialog::getOpenFileName(this, "Set domain from file extent", {}, "All files (*)");
    if (path.isEmpty()) return;
    try {
        const auto extent = readFileExtent(path.toStdString());
        setDomainToExtent(extent.crs, extent.bounds);
    } catch (const std::exception& error) { QMessageBox::critical(this, "Could not read file extent", error.what()); }
}

void DomainForm::setDomainToExtent(const Crs& extentCrs, Bounds2D bounds) {
    const auto id = selectedDomainId();
    if (!project_ || !id) return;
    auto& domain = project_->domains.domains().at(static_cast<std::size_t>(*id - 1));

    if (*id == 1) {
        if (!isValid(resolution_)) return;
        const auto projectionId = projection_->currentData().toString().toStdString();
        Crs domainCrs = Crs::lonLat();
        if (projectionId == "lambert") {
            if (!(isValid(trueLat1_) && isValid(trueLat2_) && isValid(standLon_))) throw UserError("Incomplete projection definition.");
            const double originLat = isValid(centerLat_) ? centerLat_->text().toDouble() : 0.0;
            domainCrs = Crs::lambert(trueLat1_->text().toDouble(), trueLat2_->text().toDouble(), {standLon_->text().toDouble(), originLat});
        } else if (projectionId == "polar") {
            if (!(isValid(trueLat1_) && isValid(standLon_))) throw UserError("Incomplete projection definition.");
            domainCrs = Crs::polar(trueLat1_->text().toDouble(), standLon_->text().toDouble());
        } else if (projectionId == "mercator") {
            if (!isValid(trueLat1_)) throw UserError("Incomplete projection definition.");
            const double originLon = isValid(centerLon_) ? centerLon_->text().toDouble() : 0.0;
            domainCrs = Crs::mercator(trueLat1_->text().toDouble(), originLon);
        }
        const double resolution = resolution_->text().toDouble();
        const auto domainBounds = extentCrs.transformBbox(bounds, domainCrs);
        const int cols = static_cast<int>(std::ceil((domainBounds.maxX - domainBounds.minX) / resolution));
        const int rows = static_cast<int>(std::ceil((domainBounds.maxY - domainBounds.minY) / resolution));
        if (cols > kMaxReasonableDimension || rows > kMaxReasonableDimension)
            throw UserError("That extent would need an unreasonably large grid - check you haven't zoomed out to (near) the whole world.");
        const double centerX = domainBounds.minX + (domainBounds.maxX - domainBounds.minX) / 2.0;
        const double centerY = domainBounds.minY + (domainBounds.maxY - domainBounds.minY) / 2.0;
        const auto centerLonLat = domainCrs.toLonLat({centerX, centerY});
        centerLon_->setText(QString::number(centerLonLat.lon));
        centerLat_->setText(QString::number(centerLonLat.lat));
        columns_->setText(QString::number(cols));
        rows_->setText(QString::number(rows));
    } else {
        if (!isValid(ratio_)) return;
        try { project_->domains.fillDomains(); } catch (const UserError& error) { throw UserError(QString("Configure the parent domain first: %1").arg(error.what()).toStdString()); }
        auto& parent = project_->domains.domains().at(static_cast<std::size_t>(domain.parentId - 1));
        if (!parent.bounds) throw UserError("Configure the parent domain first.");
        const int ratio = ratio_->text().toInt();
        const double ownDx = parent.dx / ratio, ownDy = parent.dy / ratio;
        const auto projection = project_->domains.projection();
        const auto domainBounds = extentCrs.transformBbox(bounds, projection);
        const int cols = static_cast<int>(std::ceil((domainBounds.maxX - domainBounds.minX) / ownDx));
        const int rows = static_cast<int>(std::ceil((domainBounds.maxY - domainBounds.minY) / ownDy));
        if (cols > kMaxReasonableDimension || rows > kMaxReasonableDimension)
            throw UserError("That extent would need an unreasonably large grid - check you haven't zoomed out to (near) the whole world.");
        const int paddingLeft = static_cast<int>(std::lround((domainBounds.minX - parent.bounds->minX) / parent.dx));
        const int paddingBottom = static_cast<int>(std::lround((domainBounds.minY - parent.bounds->minY) / parent.dy));
        paddingLeft_->setText(QString::number(paddingLeft));
        paddingBottom_->setText(QString::number(paddingBottom));
        columns_->setText(QString::number(cols));
        rows_->setText(QString::number(rows));
    }
    applySelectedDomainFields(true);
}

void DomainForm::redraw(bool zoomOut) {
    if (!project_ || project_->domains.domains().empty()) { map_->clearVectorOverlayGroup("domains"); return; }
    const auto overlays = computeDomainOverlays(project_->domains);
    if (overlays.empty()) { map_->clearVectorOverlayGroup("domains"); return; }
    map_->setVectorOverlayGroup("domains", overlays, kVectorOverlayZ);
    if (zoomOut && active_) {
        if (const auto bounds = domainLonLatBounds(overlays)) map_->zoomToBounds(bounds->first, bounds->second);
    }
}
}  // namespace wrftools
