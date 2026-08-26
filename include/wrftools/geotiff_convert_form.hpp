#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace wrftools {

// The "Convert" tab: a GUI over convert_geotiff_lib's two conversion
// directions (GeoTIFF -> WPS geogrid and back), ported from
// convert_geotiff's FLTK ConvertWindow. Runs each conversion on a
// background std::thread so the UI stays responsive, marshaling progress/
// completion back to the GUI thread via QMetaObject::invokeMethod - Qt's
// analogue of ConvertWindow's Fl::lock()/Fl::awake() pairing, and simpler
// than it since a queued invokeMethod call already does the marshaling.
class GeotiffConvertForm final : public QWidget {
public:
    explicit GeotiffConvertForm(QWidget* parent = nullptr);
    ~GeotiffConvertForm() override;

    // Action methods a real button/browse dialog would invoke - public
    // (test-only surface, mirrors this project's other forms) so tests can
    // drive them without a QFileDialog/QTest::mouseClick. setInputPath/
    // setOutputDirectory/setOutputTiffPath bypass the file pickers the way
    // browse_input()/browse_output()/browse_output_tiff() use them.
    void setInputPath(const QString& path);
    void setOutputDirectory(const QString& path);
    void setOutputTiffPath(const QString& path);
    // Validates the form and, if valid, starts the background conversion.
    // Throws UserError on any invalid/incomplete field - matching this
    // project's applyFieldsToSelectedLayer/applySelectedDomainFields
    // convention - so tests can assert on the failure directly with
    // CHECK_THROWS_AS instead of going through a blocking QMessageBox.
    // Public because of that convention; the Convert button itself is
    // wired to startConversionFromSignal() below, which catches and shows
    // the dialog for a real click.
    void runConversion();
    [[nodiscard]] bool isRunning() const noexcept { return running_; }

    // Test-facing widget accessors only - production code (this class's
    // own .cpp) never needs these, it holds the pointers directly.
    [[nodiscard]] QComboBox* directionCombo() const noexcept { return direction_; }
    [[nodiscard]] QLineEdit* inputPathField() const noexcept { return inputPath_; }
    [[nodiscard]] QLineEdit* outputDirectoryField() const noexcept { return outputDirectory_; }
    [[nodiscard]] QLineEdit* outputTiffField() const noexcept { return outputTiff_; }
    [[nodiscard]] QCheckBox* categoricalCheck() const noexcept { return categorical_; }
    [[nodiscard]] QLineEdit* categoriesField() const noexcept { return categories_; }
    [[nodiscard]] QLineEdit* borderWidthField() const noexcept { return borderWidth_; }
    [[nodiscard]] QLineEdit* tileSizeField() const noexcept { return tileSize_; }
    [[nodiscard]] QComboBox* wordSizeCombo() const noexcept { return wordSize_; }
    [[nodiscard]] QCheckBox* unsignedCheck() const noexcept { return unsigned_; }
    [[nodiscard]] QLineEdit* scaleField() const noexcept { return scale_; }
    [[nodiscard]] QLineEdit* missingField() const noexcept { return missing_; }
    [[nodiscard]] QLineEdit* unitsField() const noexcept { return units_; }
    [[nodiscard]] QLineEdit* descriptionField() const noexcept { return description_; }
    [[nodiscard]] QPushButton* convertButton() const noexcept { return convertButton_; }
    [[nodiscard]] QProgressBar* progressBar() const noexcept { return progress_; }
    [[nodiscard]] QPlainTextEdit* logView() const noexcept { return log_; }

private:
    void startConversionFromSignal();
    void browseInput();
    void browseOutputDirectory();
    void browseOutputTiff();
    void updateDirection();
    [[nodiscard]] bool isReverse() const;
    void setRunning(bool running);
    void logLine(const QString& line);
    // Spawns the worker thread running `job`, wiring up the common
    // start/finish UI state, error logging, and alert-on-failure behavior
    // shared by both conversion directions. `job` runs on the worker thread
    // and should throw on failure. Mirrors ConvertWindow::run_worker.
    void runWorker(std::function<void()> job);

    QComboBox* direction_{};
    QLineEdit* inputPath_{};
    QPushButton* browseInputButton_{};
    QLabel* inputPathLabel_{};
    QLineEdit* outputDirectory_{};
    QPushButton* browseOutputButton_{};
    QWidget* outputDirectoryLabel_{};
    QLineEdit* outputTiff_{};
    QPushButton* browseOutputTiffButton_{};
    QWidget* outputTiffLabel_{};
    QCheckBox* categorical_{};
    QLineEdit* categories_{};
    QLineEdit* borderWidth_{};
    QLineEdit* tileSize_{};
    QComboBox* wordSize_{};
    QCheckBox* unsigned_{};
    QLineEdit* scale_{};
    QLineEdit* missing_{};
    QLineEdit* units_{};
    QLineEdit* description_{};
    QPushButton* convertButton_{};
    QProgressBar* progress_{};
    QPlainTextEdit* log_{};

    std::thread worker_;
    std::atomic<bool> running_{false};
};

}  // namespace wrftools
