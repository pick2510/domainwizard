#pragma once

#include <atomic>
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
    explicit ReprojectForm(QWidget* parent = nullptr);
    ~ReprojectForm() override;

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
    [[nodiscard]] QListWidget* variablesList() const noexcept { return variables_; }
    [[nodiscard]] QComboBox* resamplingCombo() const noexcept { return resampling_; }
    [[nodiscard]] QCheckBox* mergeSeriesCheck() const noexcept { return mergeSeries_; }
    [[nodiscard]] QCheckBox* nearestForCategoricalCheck() const noexcept { return nearestForCategorical_; }
    [[nodiscard]] QLineEdit* outputDirectoryField() const noexcept { return outputDirectory_; }
    [[nodiscard]] QPushButton* runButton() const noexcept { return runButton_; }
    [[nodiscard]] QPushButton* cancelButton() const noexcept { return cancelButton_; }
    [[nodiscard]] QProgressBar* progressBar() const noexcept { return progress_; }
    [[nodiscard]] QPlainTextEdit* logView() const noexcept { return log_; }
    [[nodiscard]] QListWidget* resultsList() const noexcept { return results_; }

private:
    void startReprojectFromSignal();
    void browseInputFiles();
    void browseOutputDirectory();
    void refreshVariables();
    void setRunning(bool running);
    void logLine(const QString& line);
    void cancel();
    void handleWorkerOutput();
    void handleWorkerFinished(int exitCode, int exitStatus);

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
