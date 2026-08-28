#pragma once

#include "wrftools/crs.hpp"
#include "wrftools/derived_variable.hpp"

#include <atomic>
#include <map>
#include <optional>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;

namespace wrftools {

class TileMapWidget;

// The "Reproject" tab: reprojects a single wrfout/geo_em file, or a whole
// wrfout series, to an arbitrary EPSG code as CF-1.7 NetCDF (see
// reproject.hpp). Unlike LczForm, this does NOT run its pipeline inline on
// the GUI thread and does NOT use a background std::thread within this
// process either - both share the same underlying hazard documented at
// lcz_form.hpp:20-40 (netCDF-C/GDAL/HDF5 gets thread-affined to whichever
// OS thread first touches it, and touching it again from a second thread
// afterward deadlocks the whole process on at least one real
// configuration). A merged multi-hundred-file export is also far too long
// to accept blocking the GUI, which is the tradeoff the LCZ tab made for
// its much shorter pipeline.
//
// Instead, runReproject() launches `wrftools_reproject_worker` (see
// reproject_worker.cpp) as a separate QProcess: a small executable linked
// only against wrftools_core, so it never shares GDAL/HDF5 state with this
// GUI process, and the deadlock condition cannot occur. The job is written
// to a temporary JSON file and the worker's stdout is parsed line by line
// (PROGRESS/DONE/ERROR - see reproject_worker.cpp's own protocol comment)
// to drive the progress bar and log view, entirely through Qt's normal
// event loop - runReproject() itself returns immediately after validating
// the form and starting the process; it does not block.
class ReprojectForm final : public QWidget {
public:
    // map is shared with every other tab (Domains/View/...), same pattern
    // as DomainForm/ViewForm - see setActive.
    explicit ReprojectForm(TileMapWidget* map, QWidget* parent = nullptr);
    ~ReprojectForm() override;

    // Gates whether this tab's Area-of-Interest rectangle is the map's
    // currently drag/resize-enabled overlay - called from MainWindow's
    // QTabWidget::currentChanged, mirroring DomainForm::setActive. Without
    // this, whichever tab constructed last would permanently own the map's
    // drag/resize handlers regardless of which tab is actually showing.
    void setActive(bool active);

    // Bypass the file pickers, mirroring LczForm's setLczFile/setWrfFile -
    // so tests can drive the form without a QFileDialog.
    void setInputPaths(const QStringList& paths);
    void setOutputDirectory(const QString& path);

    // Validates every field, then starts the worker process asynchronously
    // and returns - it does NOT wait for the run to finish. Any validation
    // UserError (empty inputs, no variables checked, an unparsable EPSG or
    // grid field, a missing output directory, ...) is thrown synchronously
    // before anything is launched, so tests can CHECK_THROWS_AS(...,
    // UserError) directly; a failure that only the worker itself discovers
    // (e.g. an unresolvable EPSG code) instead surfaces later as an ERROR
    // line, logged and shown via QMessageBox, with isRunning() already back
    // to false by the time that happens.
    void runReproject();
    [[nodiscard]] bool isRunning() const noexcept { return running_; }

    // Test-facing widget accessors only.
    [[nodiscard]] QLineEdit* inputSummaryField() const noexcept { return inputSummary_; }
    [[nodiscard]] QLineEdit* epsgField() const noexcept { return epsg_; }
    [[nodiscard]] QLineEdit* pixelSizeXField() const noexcept { return pixelSizeX_; }
    [[nodiscard]] QLineEdit* pixelSizeYField() const noexcept { return pixelSizeY_; }
    [[nodiscard]] QLineEdit* extentMinXField() const noexcept { return extentMinX_; }
    [[nodiscard]] QLineEdit* extentMinYField() const noexcept { return extentMinY_; }
    [[nodiscard]] QLineEdit* extentMaxXField() const noexcept { return extentMaxX_; }
    [[nodiscard]] QLineEdit* extentMaxYField() const noexcept { return extentMaxY_; }
    [[nodiscard]] QListWidget* variablesList() const noexcept { return variables_; }
    [[nodiscard]] QPlainTextEdit* derivedVariablesScriptEdit() const noexcept { return derivedVariablesScript_; }
    [[nodiscard]] QLabel* derivedVariablesStatusLabel() const noexcept { return derivedVariablesStatus_; }
    [[nodiscard]] QComboBox* resamplingCombo() const noexcept { return resampling_; }
    [[nodiscard]] QCheckBox* mergeSeriesCheck() const noexcept { return mergeSeries_; }
    [[nodiscard]] QCheckBox* nearestForCategoricalCheck() const noexcept { return nearestForCategorical_; }
    [[nodiscard]] QLineEdit* outputDirectoryField() const noexcept { return outputDirectory_; }
    [[nodiscard]] QPushButton* runButton() const noexcept { return runButton_; }
    [[nodiscard]] QPushButton* cancelButton() const noexcept { return cancelButton_; }
    [[nodiscard]] QProgressBar* progressBar() const noexcept { return progress_; }
    [[nodiscard]] QPlainTextEdit* logView() const noexcept { return log_; }
    [[nodiscard]] QListWidget* resultsList() const noexcept { return results_; }
    [[nodiscard]] QPushButton* resetAoiButton() const noexcept { return resetAoiButton_; }
    [[nodiscard]] const std::optional<Bounds2D>& domainBoundsLonLat() const noexcept { return domainBoundsLonLat_; }
    [[nodiscard]] const std::optional<Bounds2D>& aoiBoundsLonLat() const noexcept { return aoiBoundsLonLat_; }

private:
    void startReprojectFromSignal();
    void browseInputFiles();
    void browseOutputDirectory();
    void refreshVariables();
    // Re-parses derivedVariablesScript_'s current text against
    // sourceShapes_ (rebuilt by refreshVariables()) and shows either "N
    // derived variable(s): ..." or the UserError's message in
    // derivedVariablesStatus_ - called on every edit, so purely a live
    // preview; the actual validation that blocks a run happens again in
    // runReproject() itself via the same parseDerivedVariables() call.
    void updateDerivedVariablesStatus();
    void setRunning(bool running);
    void logLine(const QString& line);
    void cancel();
    void handleWorkerOutput();
    void handleWorkerFinished(int exitCode, int exitStatus);

    // Draws the shaded domain footprint ("reproject-domain") and the
    // draggable/resizable AOI rectangle ("reproject-aoi") from
    // domainBoundsLonLat_/aoiBoundsLonLat_ - called after every change to
    // either.
    void updateMapOverlays();
    // Clears any user crop: AOI reverts to the full domain footprint and
    // the extent fields go back to "auto" (empty).
    void resetAoiToFullDomain();
    // Re-derives the Extent min/max fields (in the TARGET CRS) from
    // aoiBoundsLonLat_ - called after every AOI drag/resize move and
    // whenever the target EPSG changes, since the fields' units depend on
    // it. A no-op until the user has actually dragged/resized the AOI at
    // least once (aoiOverridden_), so a plain EPSG edit on a freshly loaded
    // file doesn't silently populate "auto" fields with the domain's own
    // coarse WGS84 bounding box.
    void applyAoiToExtentFields();
    void onAoiDragStart(std::size_t overlayIndex, LonLat pressLonLat);
    void onAoiDragMove(std::size_t overlayIndex, LonLat currentLonLat);
    void onAoiDragEnd();
    void onAoiResizeStart(std::size_t overlayIndex, std::size_t handleIndex, LonLat pressLonLat);
    void onAoiResizeMove(std::size_t overlayIndex, std::size_t handleIndex, LonLat currentLonLat);
    void onAoiResizeEnd();

    TileMapWidget* map_{};
    bool active_{false};
    std::optional<Bounds2D> domainBoundsLonLat_;
    std::optional<Bounds2D> aoiBoundsLonLat_;
    bool aoiOverridden_{false};
    struct AoiDragState { LonLat pressLonLat; Bounds2D startBounds; };
    std::optional<AoiDragState> aoiDrag_;
    struct AoiResizeState { LonLat anchor; };
    std::optional<AoiResizeState> aoiResize_;
    QPushButton* resetAoiButton_{};

    QLineEdit* inputSummary_{};
    QPushButton* browseInputsButton_{};
    QLabel* structureLabel_{};
    QStringList inputPaths_;

    QLineEdit* epsg_{};
    QLineEdit* pixelSizeX_{};
    QLineEdit* pixelSizeY_{};
    QLineEdit* extentMinX_{};
    QLineEdit* extentMinY_{};
    QLineEdit* extentMaxX_{};
    QLineEdit* extentMaxY_{};

    QListWidget* variables_{};
    QLineEdit* variableFilter_{};
    QPushButton* selectAllButton_{};
    QPushButton* selectNoneButton_{};

    // Every currently-open input file's own RAW (undestaggered) variable
    // shapes, by name - rebuilt by refreshVariables(), fed to
    // parseDerivedVariables() by both updateDerivedVariablesStatus() (live
    // preview) and runReproject() (the real validation).
    std::map<std::string, VariableShape> sourceShapes_;
    QPlainTextEdit* derivedVariablesScript_{};
    QLabel* derivedVariablesStatus_{};

    QComboBox* resampling_{};
    QCheckBox* mergeSeries_{};
    QCheckBox* nearestForCategorical_{};
    QLineEdit* outputDirectory_{};
    QPushButton* browseOutputButton_{};

    QPushButton* runButton_{};
    QPushButton* cancelButton_{};
    QProgressBar* progress_{};
    QPlainTextEdit* log_{};
    QListWidget* results_{};

    QProcess* worker_{};
    QString jobFilePath_;
    std::atomic<bool> running_{false};
};

}  // namespace wrftools
