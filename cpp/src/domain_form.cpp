#include "wrftools/domain_form.hpp"
#include "wrftools/wps_namelist.hpp"

#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

namespace wrftools {
DomainForm::DomainForm(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* import = new QPushButton("Import namelist.wps…", this);
    auto* exportFile = new QPushButton("Export namelist.wps…", this);
    auto* add = new QPushButton("Add Child Domain", this);
    auto* remove = new QPushButton("Remove Domain", this);
    tree_ = new QTreeWidget(this); tree_->setHeaderLabels({"Domain", "Parent", "Grid", "Ratio"});
    layout->addWidget(import); layout->addWidget(exportFile); layout->addWidget(add); layout->addWidget(remove); layout->addWidget(tree_);
    connect(import, &QPushButton::clicked, this, [this] { importNamelist(); });
    connect(exportFile, &QPushButton::clicked, this, [this] { exportNamelist(); });
    connect(add, &QPushButton::clicked, this, [this] { addChild(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeSelected(); });
}
void DomainForm::addChild() {
    if (!project_ || !tree_->currentItem()) return;
    const int parentId = tree_->currentItem()->text(0).section(' ', 1).toInt();
    auto& domains = project_->domains.domains();
    const auto& parent = domains.at(static_cast<std::size_t>(parentId - 1));
    domains.push_back({.id = static_cast<int>(domains.size()) + 1, .parentId = parentId, .ratio = 3, .paddingLeft = 0, .paddingBottom = 0, .columns = std::max(1, parent.columns / 3), .rows = std::max(1, parent.rows / 3), .bounds = std::nullopt});
    rebuildTree();
}
void DomainForm::removeSelected() {
    if (!project_ || !tree_->currentItem()) return;
    const int id = tree_->currentItem()->text(0).section(' ', 1).toInt();
    if (id == 1) { QMessageBox::information(this, "Cannot remove domain", "The root domain cannot be removed."); return; }
    project_->domains.removeSubtree(id);
    rebuildTree();
}
void DomainForm::importNamelist() {
    const auto path = QFileDialog::getOpenFileName(this, "Import WPS namelist", {}, "WPS namelist (*)");
    if (path.isEmpty()) return;
    try { project_ = readWpsNamelist(path.toStdString()); rebuildTree(); }
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
    tree_->clear();
    if (!project_) return;
    std::map<int, QTreeWidgetItem*> items;
    for (const auto& domain : project_->domains.domains()) {
        auto* item = new QTreeWidgetItem({QString("Domain %1").arg(domain.id), QString::number(domain.parentId), QString("%1 × %2").arg(domain.columns).arg(domain.rows), QString::number(domain.ratio)});
        items.emplace(domain.id, item);
        if (domain.id == 1) tree_->addTopLevelItem(item); else items.at(domain.parentId)->addChild(item);
    }
    tree_->expandAll();
}
}  // namespace wrftools
