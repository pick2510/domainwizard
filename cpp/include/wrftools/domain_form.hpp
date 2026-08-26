#pragma once

#include "wrftools/wps_namelist.hpp"

#include <optional>
#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace wrftools {
class TileMapWidget;

class DomainForm final : public QWidget {
public:
    explicit DomainForm(TileMapWidget* map, QWidget* parent = nullptr);
    // Gates whether redrawing the outlines also re-centers the shared map -
    // set false while another tab (View) owns the camera. Mirrors
    // domainform.py's set_active.
    void setActive(bool active);
    // Loads a project directly (bypassing the file-picker importNamelist()
    // uses) - the entry point both importNamelist() and tests use.
    void setProject(WpsProject project);
    [[nodiscard]] const std::optional<WpsProject>& project() const noexcept { return project_; }
    [[nodiscard]] QTreeWidget* domainTree() const noexcept { return tree_; }

    // Action methods a real button click would invoke, plus the "Position
    // within Parent" fields they read - public (test-only surface, mirrors
    // Python's on_add_domain_button_clicked()/on_remove_domain_button_
    // clicked()/_apply_selected_domain_fields() and padding_left/
    // padding_bottom) so tests can drive them without a QTest::mouseClick.
    void addChild();
    void removeSelected();
    // Validates and writes the visible fields back into the selected
    // domain, then calls fillDomains(). Returns false (or, if
    // raiseOnInvalid, throws UserError) on any invalid/incomplete field or
    // a geometry that doesn't fit its parent. Mirrors
    // domainform.py's _apply_selected_domain_fields.
    bool applySelectedDomainFields(bool raiseOnInvalid);
    [[nodiscard]] QLineEdit* paddingLeftField() const noexcept { return paddingLeft_; }
    [[nodiscard]] QLineEdit* paddingBottomField() const noexcept { return paddingBottom_; }

private:
    void importNamelist();
    void exportNamelist();
    void rebuildTree();
    void updateSelection();
    void updatePanelVisibility();
    void updateProjectionParamVisibility(const QString& projectionId);
    void populatePropertiesPanel();
    [[nodiscard]] std::optional<int> selectedDomainId() const;
    void setDomainToExtent(const Crs& extentCrs, Bounds2D bounds);
    void onSetMapExtentClicked();
    void onSetFileExtentClicked();
    void redraw(bool zoomOut);

    TileMapWidget* map_;
    std::optional<WpsProject> project_;
    bool active_{true};

    QTreeWidget* tree_{};
    QLabel* setFromLabel_{};

    QGroupBox* mapTypeGroup_{};
    QComboBox* projection_{};
    QLineEdit* trueLat1_{};
    QLineEdit* trueLat2_{};
    QLineEdit* standLon_{};

    QGroupBox* resolutionGroup_{};
    QLineEdit* resolution_{};

    QGroupBox* nestingGroup_{};
    QLineEdit* ratio_{};

    QGroupBox* extentCalcGroup_{};

    QGroupBox* centerGroup_{};
    QLineEdit* centerLon_{};
    QLineEdit* centerLat_{};

    QGroupBox* positionGroup_{};
    QLineEdit* paddingLeft_{};
    QLineEdit* paddingBottom_{};

    QGroupBox* gridExtentGroup_{};
    QLineEdit* columns_{};
    QLineEdit* rows_{};
};
}  // namespace wrftools
