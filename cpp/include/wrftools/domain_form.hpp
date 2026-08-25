#pragma once

#include <QWidget>
#include <optional>
#include "wrftools/wps_namelist.hpp"

class QTreeWidget;

namespace wrftools {
class DomainForm final : public QWidget {
public:
    explicit DomainForm(QWidget* parent = nullptr);
private:
    void importNamelist();
    void exportNamelist();
    void addChild();
    void removeSelected();
    void rebuildTree();
    std::optional<WpsProject> project_;
    QTreeWidget* tree_{};
};
}  // namespace wrftools
