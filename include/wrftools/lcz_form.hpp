#pragma once

#include <atomic>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace wrftools {

// The "LCZ" tab: a GUI over the Stage 2-4 LCZ pipeline (removeUrban ->
// createLczParamsFile -> createLczExtentFile -> expandLandCatParents ->
// checksAndCleaning), ported from add_wrf_version's main() (w2w.py:57-239).
//
// Unlike GeotiffConvertForm, this form runs its entire pipeline
// synchronously on the GUI thread, NOT on a background std::thread -
// deliberately, not as an oversight. GeotiffConvertForm's own worker
// thread is safe only because convert_geotiff_lib is GDAL-free by design
// (writes via libtiff/libgeotiff directly); this pipeline is built on
// NetcdfFile (netCDF-C) and GDAL warps, and on at least one real
// configuration (Fedora's system libhdf5, built without
// --enable-threadsafe) using netCDF-C/GDAL from a second OS thread AFTER
// the main/GUI thread has already touched them - even with every handle
// already fully closed, non-concurrently - deadlocks the process. Confirmed
// by direct reproduction, not theoretical: since main.cpp's own
// GDALAllRegister() call (and ordinary GDAL use elsewhere in this app, all
// on the GUI thread) means GDAL/HDF5 state is *always* already
// thread-affined to the GUI thread by the time a user opens this tab, no
// background thread here can ever be safe on such a build - moving only
// checkLczIntegrity off the GUI thread would not have helped. runLcz()
// calls QCoreApplication::processEvents() between pipeline stages instead,
// so the UI still repaints/responds to a resize between stages even though
// it's fully blocked while inside any single one (the same tradeoff this
// project already accepted for checkLczIntegrity alone, now extended to
// the whole run - see PORT_W2W.MD Stage 5's follow-up note).
//
// Unlike w2w.py's CLI, this form has no separate io_dir argument: every
// output path this pipeline writes is derived directly from the target
// geo_em file's own path (matching Info.from_argparse's own os.path.join(
// io_dir, wrf_file.replace(...)) construction when io_dir is simply that
// file's parent directory) - the LCZ GeoTIFF need not sit in the same
// directory. A deliberate simplification over the CLI's own two-path
// requirement, not a fidelity loss: every wrftools/lcz.hpp function already
// takes full paths, not io_dir-relative ones.
//
// One other deliberate difference from w2w.py's own `_get_lcz_band`: this
// form does not auto-detect an LCZ Generator ("lczFilter") GeoTIFF and
// silently switch to band 1 - the LCZ band field is a plain, always-honored
// user value defaulting to 0. A user with an LCZ Generator map sets it to 1
// themselves. See PORT_W2W.MD Stage 5 for the full rationale.
class LczForm final : public QWidget {
public:
    explicit LczForm(QWidget* parent = nullptr);
    ~LczForm() override;

    // Bypass the file pickers, mirroring GeotiffConvertForm's
    // setInputPath/setOutputDirectory - so tests can drive the form without
    // a QFileDialog.
    void setLczFile(const QString& path);
    void setWrfFile(const QString& path);
    void setCustomUcpTable(const QString& path);

    // Validates every field, then runs the entire pipeline synchronously
    // (see the class comment for why it isn't backgrounded) - any
    // UserError, whether from field validation or from a pipeline stage
    // itself, propagates out of this call. Public so tests can assert on
    // failures directly with CHECK_THROWS_AS; the Run button itself is
    // wired to startLczFromSignal(), which catches and shows a
    // QMessageBox for a real click.
    void runLcz();
    [[nodiscard]] bool isRunning() const noexcept { return running_; }

    // Test-facing widget accessors only - production code (this class's
    // own .cpp) never needs these, it holds the pointers directly.
    [[nodiscard]] QLineEdit* lczFileField() const noexcept { return lczFile_; }
    [[nodiscard]] QLineEdit* wrfFileField() const noexcept { return wrfFile_; }
    [[nodiscard]] QComboBox* wrfVersionCombo() const noexcept { return wrfVersion_; }
    [[nodiscard]] QLineEdit* builtLczField() const noexcept { return builtLcz_; }
    [[nodiscard]] QLineEdit* lczBandField() const noexcept { return lczBand_; }
    [[nodiscard]] QLineEdit* frcThresholdField() const noexcept { return frcThreshold_; }
    [[nodiscard]] QLineEdit* npixNlcField() const noexcept { return npixNlc_; }
    [[nodiscard]] QLineEdit* npixAreaField() const noexcept { return npixArea_; }
    [[nodiscard]] QLineEdit* customUcpTableField() const noexcept { return customUcpTable_; }
    [[nodiscard]] QPushButton* runButton() const noexcept { return runButton_; }
    [[nodiscard]] QProgressBar* progressBar() const noexcept { return progress_; }
    [[nodiscard]] QPlainTextEdit* logView() const noexcept { return log_; }
    [[nodiscard]] QListWidget* resultsList() const noexcept { return results_; }
    [[nodiscard]] QLabel* nbuiMaxLabel() const noexcept { return nbuiMaxLabel_; }

private:
    void startLczFromSignal();
    void browseLczFile();
    void browseWrfFile();
    void browseCustomUcpTable();
    void setRunning(bool running);
    void logLine(const QString& line);

    QLineEdit* lczFile_{};
    QPushButton* browseLczButton_{};
    QLineEdit* wrfFile_{};
    QPushButton* browseWrfButton_{};
    QComboBox* wrfVersion_{};
    QLineEdit* builtLcz_{};
    QLineEdit* lczBand_{};
    QLineEdit* frcThreshold_{};
    QLineEdit* npixNlc_{};
    QLineEdit* npixArea_{};
    QLineEdit* customUcpTable_{};
    QPushButton* browseCustomUcpButton_{};
    QPushButton* runButton_{};
    QProgressBar* progress_{};
    QPlainTextEdit* log_{};
    QListWidget* results_{};
    QLabel* nbuiMaxLabel_{};

    std::atomic<bool> running_{false};
};

}  // namespace wrftools
