#include "wrftools/domain_form.hpp"
#include "wrftools/domain_overlay.hpp"
#include "wrftools/error.hpp"
#include "wrftools/tile_map_widget.hpp"
#include "wrftools/wps_namelist.hpp"

#include <QComboBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QIntValidator>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <array>
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

DomainForm::DomainForm(TileMapWidget* map, QWidget* parent) : QWidget(parent), map_(map), project_(std::in_place) {
    auto* layout = new QVBoxLayout(this);
    auto* import = new QPushButton("Import namelist.wps…", this);
    auto* exportFile = new QPushButton("Export namelist.wps…", this);
    layout->addWidget(import); layout->addWidget(exportFile);

    tree_ = new QTreeWidget(this); tree_->setHeaderHidden(true);
    addDomainButton_ = new QPushButton("Add Root Domain", this);
    removeDomainButton_ = new QPushButton("Remove Domain", this);
    layout->addWidget(tree_); layout->addWidget(addDomainButton_); layout->addWidget(removeDomainButton_);

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
    extentLayout->addWidget(setFromMap);
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
    connect(addDomainButton_, &QPushButton::clicked, this, [this] { addChild(); });
    connect(removeDomainButton_, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { updateSelection(); });
    connect(projection_, &QComboBox::currentIndexChanged, this, [this] { updateProjectionParamVisibility(projection_->currentData().toString()); applySelectedDomainFields(false); });
    for (auto* field : {trueLat1_, trueLat2_, standLon_, resolution_, centerLon_, centerLat_, ratio_, paddingLeft_, paddingBottom_, columns_, rows_})
        connect(field, &QLineEdit::editingFinished, this, [this] { applySelectedDomainFields(false); });
    connect(setFromMap, &QPushButton::clicked, this, [this] { onSetMapExtentClicked(); });

    map_->setOverlayDragHandlers(
        [this](std::size_t index, LonLat lonLat) { onDomainOverlayDragStart(index, lonLat); },
        [this](std::size_t index, LonLat lonLat) { onDomainOverlayDragMove(index, lonLat); },
        [this] { onDomainOverlayDragEnd(); });
    map_->setOverlayResizeHandlers(
        [this](std::size_t index, std::size_t handle, LonLat lonLat) { onDomainOverlayResizeStart(index, handle, lonLat); },
        [this](std::size_t index, std::size_t handle, LonLat lonLat) { onDomainOverlayResizeMove(index, handle, lonLat); },
        [this] { onDomainOverlayResizeEnd(); });
    map_->setDraggableVectorOverlayGroup(active_ ? "domains" : QString());

    updatePanelVisibility();
}

void DomainForm::setActive(bool active) {
    active_ = active;
    // Only this tab's own "domains" outlines are ever draggable - the map
    // is shared with View, whose raster layers live in a different overlay
    // group ("view-rasters") that's never marked draggable, but gating this
    // on active_ too means a click on the map while View owns it always
    // pans/zooms, never repositions a domain sitting underneath.
    map_->setDraggableVectorOverlayGroup(active_ ? "domains" : QString());
}

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

    const bool hasDomains = project_ && !project_->domains.domains().empty();
    removeDomainButton_->setEnabled(hasSelection);
    addDomainButton_->setText(hasDomains ? "Add Child Domain" : "Add Root Domain");
    // No domains yet: nothing to select, so the button (which creates the
    // root) is always enabled. Once domains exist, adding one needs a
    // selected parent. Mirrors domainform.py's add_domain_button wiring.
    addDomainButton_->setEnabled(hasSelection || !hasDomains);
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
    if (!project_) return;
    auto& domains = project_->domains.domains();
    if (domains.empty()) {
        // No project loaded yet (or an imported one had none) - this click
        // creates the root domain itself, with the same defaults as
        // domainform.py's on_add_domain_button_clicked: lat-lon projection,
        // a 0.1x0.1 (degree) cell, a 10x10 grid, centered on 0N/0E. The
        // user edits these via the Map Type/Resolution/Center Point panels
        // afterward, same as any other field edit.
        domains.push_back({.id = 1, .parentId = 1, .columns = 10, .rows = 10, .dx = 0.1, .dy = 0.1, .bounds = std::nullopt, .mapProj = "lat-lon", .centerLon = 0.0, .centerLat = 0.0});
        rebuildTree();
        return;
    }
    if (!tree_->currentItem()) return;
    const int parentId = *selectedDomainId();
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

QTreeWidgetItem* DomainForm::findTreeItem(int domainId) const {
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->data(0, kDomainIdRole).toInt() == domainId) return *it;
    return nullptr;
}

void DomainForm::onDomainOverlayDragStart(std::size_t overlayIndex, LonLat pressLonLat) {
    if (!project_) return;
    auto& domains = project_->domains.domains();
    if (overlayIndex >= domains.size()) return;
    const auto& domain = domains[overlayIndex];

    DomainDragState state;
    state.domainId = domain.id;
    state.pressLonLat = pressLonLat;
    if (domain.id == 1) { state.startCenterLon = domain.centerLon; state.startCenterLat = domain.centerLat; }
    else { state.startPaddingLeft = domain.paddingLeft; state.startPaddingBottom = domain.paddingBottom; }
    domainDrag_ = state;

    // Select the dragged domain (even if it wasn't already) so the
    // properties panel below tracks whichever outline is actually moving.
    if (auto* item = findTreeItem(domain.id)) tree_->setCurrentItem(item);
}

void DomainForm::onDomainOverlayDragMove(std::size_t overlayIndex, LonLat currentLonLat) {
    if (!domainDrag_ || !project_) return;
    auto& domains = project_->domains.domains();
    if (overlayIndex >= domains.size()) return;
    auto& domain = domains[overlayIndex];
    if (domain.id != domainDrag_->domainId) return;  // overlay indices shifted mid-drag; ignore this tick

    if (domain.id == 1) {
        domain.centerLon = domainDrag_->startCenterLon + (currentLonLat.lon - domainDrag_->pressLonLat.lon);
        domain.centerLat = domainDrag_->startCenterLat + (currentLonLat.lat - domainDrag_->pressLonLat.lat);
    } else {
        try {
            auto& parent = domains.at(static_cast<std::size_t>(domain.parentId - 1));
            const auto projection = project_->domains.projection();
            const auto pressXy = projection.toXy(domainDrag_->pressLonLat);
            const auto currentXy = projection.toXy(currentLonLat);
            const double cellsX = (currentXy.x - pressXy.x) / parent.dx;
            const double cellsY = (currentXy.y - pressXy.y) / parent.dy;
            domain.paddingLeft = std::max(0, domainDrag_->startPaddingLeft + static_cast<int>(std::lround(cellsX)));
            domain.paddingBottom = std::max(0, domainDrag_->startPaddingBottom + static_cast<int>(std::lround(cellsY)));
        } catch (const UserError&) {
            return;  // parent/projection not (yet) configured - nothing to reposition against
        }
    }

    // Mirrors applySelectedDomainFields: refresh the visible fields and
    // redraw. redraw() -> computeDomainOverlays() swallows a UserError (a
    // geometry that doesn't yet fit its parent) by clearing the outlines
    // rather than throwing, so an in-progress drag that strays outside the
    // parent just hides outlines until it's dragged back, instead of
    // crashing or freezing the drag.
    populatePropertiesPanel();
    redraw(false);
}

void DomainForm::onDomainOverlayDragEnd() { domainDrag_.reset(); }

void DomainForm::onDomainOverlayResizeStart(std::size_t overlayIndex, std::size_t handleIndex, LonLat) {
    if (!project_) return;
    auto& domains = project_->domains.domains();
    if (overlayIndex >= domains.size()) return;
    const auto& domain = domains[overlayIndex];
    if (!domain.bounds) return;

    // Handle order is SW, SE, NE, NW (see domain_overlay.cpp); the anchor
    // is the diagonally opposite corner, i.e. two positions further around.
    static constexpr std::array<int, 4> kOppositeOf{2, 3, 0, 1};
    if (handleIndex >= kOppositeOf.size()) return;
    const auto& b = *domain.bounds;
    const std::array<Coordinate2D, 4> corners{{{b.minX, b.minY}, {b.maxX, b.minY}, {b.maxX, b.maxY}, {b.minX, b.maxY}}};

    DomainResizeState state;
    state.domainId = domain.id;
    state.anchorXy = corners[static_cast<std::size_t>(kOppositeOf[handleIndex])];
    domainResize_ = state;

    if (auto* item = findTreeItem(domain.id)) tree_->setCurrentItem(item);
}

void DomainForm::onDomainOverlayResizeMove(std::size_t overlayIndex, std::size_t, LonLat currentLonLat) {
    if (!domainResize_ || !project_) return;
    auto& domains = project_->domains.domains();
    if (overlayIndex >= domains.size()) return;
    auto& domain = domains[overlayIndex];
    if (domain.id != domainResize_->domainId) return;
    if (domain.dx <= 0 || domain.dy <= 0) return;

    try {
        const auto projection = project_->domains.projection();
        const auto currentXy = projection.toXy(currentLonLat);
        const auto& anchor = domainResize_->anchorXy;
        const double width = std::abs(currentXy.x - anchor.x);
        const double height = std::abs(currentXy.y - anchor.y);
        domain.columns = std::max(1, static_cast<int>(std::lround(width / domain.dx)));
        domain.rows = std::max(1, static_cast<int>(std::lround(height / domain.dy)));

        if (domain.id == 1) {
            // Center is always the midpoint of any two diagonal corners -
            // the anchor (fixed) and the corner now under the cursor.
            const auto newCenter = projection.toLonLat({(anchor.x + currentXy.x) / 2.0, (anchor.y + currentXy.y) / 2.0});
            domain.centerLon = newCenter.lon;
            domain.centerLat = newCenter.lat;
        } else {
            auto& parent = domains.at(static_cast<std::size_t>(domain.parentId - 1));
            if (!parent.bounds || parent.dx <= 0 || parent.dy <= 0) return;
            const double newMinX = std::min(anchor.x, currentXy.x);
            const double newMinY = std::min(anchor.y, currentXy.y);
            domain.paddingLeft = std::max(0, static_cast<int>(std::lround((newMinX - parent.bounds->minX) / parent.dx)));
            domain.paddingBottom = std::max(0, static_cast<int>(std::lround((newMinY - parent.bounds->minY) / parent.dy)));
        }
    } catch (const UserError&) {
        return;  // projection not (yet) configured - nothing to resize against
    }

    populatePropertiesPanel();
    redraw(false);
}

void DomainForm::onDomainOverlayResizeEnd() { domainResize_.reset(); }

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
