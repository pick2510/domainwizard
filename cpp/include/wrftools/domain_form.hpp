#pragma once

#include "wrftools/wps_namelist.hpp"

#include <optional>
#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
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
    [[nodiscard]] QPushButton* addDomainButton() const noexcept { return addDomainButton_; }
    [[nodiscard]] QPushButton* removeDomainButton() const noexcept { return removeDomainButton_; }

    // Test-facing widget accessors only - mirror the equivalent accessors
    // already present on ViewForm; production code (this class's own .cpp)
    // never needs these, it holds the pointers directly.
    [[nodiscard]] QComboBox* projectionCombo() const noexcept { return projection_; }
    [[nodiscard]] QLineEdit* trueLat1Field() const noexcept { return trueLat1_; }
    [[nodiscard]] QLineEdit* trueLat2Field() const noexcept { return trueLat2_; }
    [[nodiscard]] QLineEdit* standLonField() const noexcept { return standLon_; }
    [[nodiscard]] QLineEdit* resolutionField() const noexcept { return resolution_; }
    [[nodiscard]] QLineEdit* ratioField() const noexcept { return ratio_; }
    [[nodiscard]] QLineEdit* centerLonField() const noexcept { return centerLon_; }
    [[nodiscard]] QLineEdit* centerLatField() const noexcept { return centerLat_; }
    [[nodiscard]] QLineEdit* columnsField() const noexcept { return columns_; }
    [[nodiscard]] QLineEdit* rowsField() const noexcept { return rows_; }

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
    [[nodiscard]] QTreeWidgetItem* findTreeItem(int domainId) const;
    // Wired to map_'s draggable-overlay hooks (see TileMapWidget::
    // setOverlayDragHandlers) so dragging a domain's outline on the map
    // repositions it exactly as typing new values into the properties
    // panel would - a root's Center Point, or a nested domain's Position
    // within Parent - and both the selection and the panel follow the drag
    // live. Moving any domain (root or nested) automatically carries its
    // descendants along for free: fillDomains() always derives a child's
    // bounds from its parent's *current* bounds plus the child's own fixed
    // padding, so nothing here needs to touch descendants explicitly.
    void onDomainOverlayDragStart(std::size_t overlayIndex, LonLat pressLonLat);
    void onDomainOverlayDragMove(std::size_t overlayIndex, LonLat currentLonLat);
    void onDomainOverlayDragEnd();

    TileMapWidget* map_;
    std::optional<WpsProject> project_;
    bool active_{true};

    // Captured once at drag start, not accumulated frame-to-frame, so a
    // nested domain's whole-cell padding rounding never compounds across
    // many small mouse-move ticks.
    struct DomainDragState {
        int domainId{};
        LonLat pressLonLat{};
        double startCenterLon{}, startCenterLat{};      // root only
        int startPaddingLeft{}, startPaddingBottom{};   // non-root only
    };
    std::optional<DomainDragState> domainDrag_;

    QTreeWidget* tree_{};
    QLabel* setFromLabel_{};
    QPushButton* addDomainButton_{};
    QPushButton* removeDomainButton_{};

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
