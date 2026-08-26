#include "wrftools/geotiff_convert_form.hpp"

#include "convert_geotiff/convert.hpp"
#include "convert_geotiff/convert_back.hpp"
#include "wrftools/error.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace wrftools {
namespace {
// Saves the process's current directory on construction and restores it on
// destruction (including via an exception) - see its use in
// GeotiffConvertForm::runConversion.
class RestoreCurrentPath {
public:
    RestoreCurrentPath() : previous_(std::filesystem::current_path()) {}
    ~RestoreCurrentPath() { std::error_code ignored; std::filesystem::current_path(previous_, ignored); }
    RestoreCurrentPath(const RestoreCurrentPath&) = delete;
    RestoreCurrentPath& operator=(const RestoreCurrentPath&) = delete;

private:
    std::filesystem::path previous_;
};
}  // namespace

GeotiffConvertForm::GeotiffConvertForm(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* directionForm = new QFormLayout;
    direction_ = new QComboBox(this);
    direction_->addItem("GeoTIFF -> geogrid");
    direction_->addItem("geogrid -> GeoTIFF");
    directionForm->addRow("Direction", direction_);
    layout->addLayout(directionForm);

    auto* pathsGroup = new QGroupBox("Files", this);
    auto* pathsForm = new QFormLayout;
    inputPath_ = new QLineEdit(this);
    inputPath_->setReadOnly(true);
    browseInputButton_ = new QPushButton("Browse...", this);
    auto* inputRow = new QWidget(this);
    auto* inputRowLayout = new QHBoxLayout(inputRow);
    inputRowLayout->setContentsMargins(0, 0, 0, 0);
    inputRowLayout->addWidget(inputPath_);
    inputRowLayout->addWidget(browseInputButton_);
    pathsForm->addRow("GeoTIFF file", inputRow);
    inputPathLabel_ = qobject_cast<QLabel*>(pathsForm->labelForField(inputRow));

    outputDirectory_ = new QLineEdit(this);
    outputDirectory_->setReadOnly(true);
    browseOutputButton_ = new QPushButton("Browse...", this);
    auto* outputDirRow = new QWidget(this);
    auto* outputDirRowLayout = new QHBoxLayout(outputDirRow);
    outputDirRowLayout->setContentsMargins(0, 0, 0, 0);
    outputDirRowLayout->addWidget(outputDirectory_);
    outputDirRowLayout->addWidget(browseOutputButton_);
    pathsForm->addRow("Output directory", outputDirRow);
    outputDirectoryLabel_ = pathsForm->labelForField(outputDirRow);

    outputTiff_ = new QLineEdit(this);
    outputTiff_->setReadOnly(true);
    browseOutputTiffButton_ = new QPushButton("Browse...", this);
    auto* outputTiffRow = new QWidget(this);
    auto* outputTiffRowLayout = new QHBoxLayout(outputTiffRow);
    outputTiffRowLayout->setContentsMargins(0, 0, 0, 0);
    outputTiffRowLayout->addWidget(outputTiff_);
    outputTiffRowLayout->addWidget(browseOutputTiffButton_);
    pathsForm->addRow("Output GeoTIFF", outputTiffRow);
    outputTiffLabel_ = pathsForm->labelForField(outputTiffRow);
    pathsGroup->setLayout(pathsForm);
    layout->addWidget(pathsGroup);

    auto* optionsGroup = new QGroupBox("Conversion Options", this);
    auto* optionsForm = new QFormLayout;
    optionsForm->setVerticalSpacing(10);
    categorical_ = new QCheckBox("Categorical data", this);
    categories_ = new QLineEdit("10", this);
    categories_->setEnabled(false);
    auto* categoricalRow = new QWidget(this);
    auto* categoricalRowLayout = new QHBoxLayout(categoricalRow);
    categoricalRowLayout->setContentsMargins(0, 0, 0, 0);
    categoricalRowLayout->addWidget(categorical_);
    categoricalRowLayout->addWidget(new QLabel("Categories:", this));
    categoricalRowLayout->addWidget(categories_);
    optionsForm->addRow(categoricalRow);

    borderWidth_ = new QLineEdit("3", this);
    optionsForm->addRow("Border width", borderWidth_);
    tileSize_ = new QLineEdit("100", this);
    optionsForm->addRow("Tile size", tileSize_);
    wordSize_ = new QComboBox(this);
    wordSize_->addItem("1"); wordSize_->addItem("2"); wordSize_->addItem("4");
    wordSize_->setCurrentIndex(1);  // "2", matching ConvertWindow's default
    optionsForm->addRow("Word size (bytes)", wordSize_);
    unsigned_ = new QCheckBox("Unsigned data", this);
    optionsForm->addRow("", unsigned_);
    scale_ = new QLineEdit("1.0", this);
    optionsForm->addRow("Scale factor", scale_);
    missing_ = new QLineEdit("0.0", this);
    optionsForm->addRow("Missing value", missing_);
    units_ = new QLineEdit("NO UNITS", this);
    optionsForm->addRow("Units", units_);
    description_ = new QLineEdit("NO DESCRIPTION", this);
    optionsForm->addRow("Description", description_);
    optionsGroup->setLayout(optionsForm);
    layout->addWidget(optionsGroup);

    convertButton_ = new QPushButton("Convert", this);
    layout->addWidget(convertButton_);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1);
    progress_->setValue(0);
    layout->addWidget(progress_);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMinimumHeight(160);
    layout->addWidget(log_);

    connect(direction_, &QComboBox::currentIndexChanged, this, [this] { updateDirection(); });
    connect(browseInputButton_, &QPushButton::clicked, this, [this] { browseInput(); });
    connect(browseOutputButton_, &QPushButton::clicked, this, [this] { browseOutputDirectory(); });
    connect(browseOutputTiffButton_, &QPushButton::clicked, this, [this] { browseOutputTiff(); });
    connect(categorical_, &QCheckBox::toggled, this, [this](bool checked) { categories_->setEnabled(checked); });
    connect(convertButton_, &QPushButton::clicked, this, [this] { startConversionFromSignal(); });

    updateDirection();
}

GeotiffConvertForm::~GeotiffConvertForm() {
    if (worker_.joinable()) worker_.join();
}

bool GeotiffConvertForm::isReverse() const { return direction_->currentIndex() == 1; }

void GeotiffConvertForm::updateDirection() {
    const bool reverse = isReverse();
    if (inputPathLabel_) inputPathLabel_->setText(reverse ? "Geogrid directory" : "GeoTIFF file");
    inputPath_->setPlaceholderText(reverse ? "Geogrid directory..." : "GeoTIFF file...");
    inputPath_->clear();
    outputDirectory_->setVisible(!reverse);
    browseOutputButton_->setVisible(!reverse);
    if (outputDirectoryLabel_) outputDirectoryLabel_->setVisible(!reverse);
    outputDirectory_->clear();
    outputTiff_->setVisible(reverse);
    browseOutputTiffButton_->setVisible(reverse);
    if (outputTiffLabel_) outputTiffLabel_->setVisible(reverse);
    outputTiff_->clear();

    // The forward-only options are meaningless for the reverse direction -
    // everything it needs is already in the geogrid index file.
    for (QWidget* widget : {static_cast<QWidget*>(categorical_), static_cast<QWidget*>(categories_), static_cast<QWidget*>(borderWidth_),
             static_cast<QWidget*>(tileSize_), static_cast<QWidget*>(wordSize_), static_cast<QWidget*>(unsigned_), static_cast<QWidget*>(scale_),
             static_cast<QWidget*>(missing_), static_cast<QWidget*>(units_), static_cast<QWidget*>(description_)})
        widget->setEnabled(!reverse);
    if (!reverse && !categorical_->isChecked()) categories_->setEnabled(false);
}

void GeotiffConvertForm::setInputPath(const QString& path) { inputPath_->setText(path); }
void GeotiffConvertForm::setOutputDirectory(const QString& path) { outputDirectory_->setText(path); }
void GeotiffConvertForm::setOutputTiffPath(const QString& path) { outputTiff_->setText(path); }

void GeotiffConvertForm::browseInput() {
    if (isReverse()) {
        const auto path = QFileDialog::getExistingDirectory(this, "Choose a geogrid directory");
        if (!path.isEmpty()) setInputPath(path);
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, "Choose a GeoTIFF file", {}, "GeoTIFF Files (*.tif *.tiff)");
    if (!path.isEmpty()) setInputPath(path);
}

void GeotiffConvertForm::browseOutputDirectory() {
    const auto path = QFileDialog::getExistingDirectory(this, "Choose an output directory", {}, QFileDialog::ShowDirsOnly);
    if (!path.isEmpty()) setOutputDirectory(path);
}

void GeotiffConvertForm::browseOutputTiff() {
    const auto path = QFileDialog::getSaveFileName(this, "Choose the output GeoTIFF path", {}, "GeoTIFF Files (*.tif *.tiff)");
    if (!path.isEmpty()) setOutputTiffPath(path);
}

void GeotiffConvertForm::setRunning(bool running) {
    running_ = running;
    convertButton_->setEnabled(!running);
    browseInputButton_->setEnabled(!running);
    browseOutputButton_->setEnabled(!running);
    browseOutputTiffButton_->setEnabled(!running);
    direction_->setEnabled(!running);
}

void GeotiffConvertForm::logLine(const QString& line) {
    log_->appendPlainText(line);
}

void GeotiffConvertForm::startConversionFromSignal() {
    try {
        runConversion();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Convert", QString::fromUtf8(error.what()));
    }
}

void GeotiffConvertForm::runConversion() {
    if (running_) return;

    if (isReverse()) {
        const auto geogridDir = inputPath_->text();
        const auto outputTiff = outputTiff_->text();
        if (geogridDir.isEmpty()) throw UserError("Please choose a geogrid directory to convert.");
        if (outputTiff.isEmpty()) throw UserError("Please choose an output GeoTIFF path.");

        progress_->setFormat("");
        const auto geogridDirStd = geogridDir.toStdString();
        const auto outputTiffStd = outputTiff.toStdString();
        runWorker([geogridDirStd, outputTiffStd] { convert_geotiff::convert_back(geogridDirStd, outputTiffStd); });
        return;
    }

    const auto filename = inputPath_->text();
    const auto outputDir = outputDirectory_->text();
    if (filename.isEmpty()) throw UserError("Please choose a GeoTIFF file to convert.");
    if (outputDir.isEmpty()) throw UserError("Please choose an output directory.");

    convert_geotiff::ConversionOptions opts;
    opts.border_width = borderWidth_->text().toInt();
    opts.tile_size = tileSize_->text().toInt();
    opts.word_size = wordSize_->currentText().toInt();
    opts.isigned = !unsigned_->isChecked();
    opts.scale = scale_->text().toFloat();
    opts.missing = missing_->text().toFloat();
    opts.units = "\"" + units_->text().toStdString() + "\"";
    opts.description = "\"" + description_->text().toStdString() + "\"";
    opts.categorical_range = categorical_->isChecked() ? categories_->text().toInt() : 0;

    if (opts.border_width < 0) throw UserError("Border width must be >= 0.");
    if (opts.tile_size <= 0 || opts.tile_size > 99999) throw UserError("Tile size must be between 1 and 99999.");
    if (opts.scale == 0.f) throw UserError("Scale factor must not be 0.");
    if (categorical_->isChecked() && opts.categorical_range <= 0) throw UserError("Number of categories must be > 0.");

    const auto filenameStd = filename.toStdString();
    const auto outputDirStd = outputDir.toStdString();
    runWorker([this, filenameStd, outputDirStd, opts] {
        // convert() writes "index" and every tile via relative paths (see
        // convert_geotiff_lib), so it has to run with the output directory
        // as the process's current directory - restore the prior directory
        // afterward (RestoreCurrentPath's destructor, even on exception),
        // since the working directory is process-global state that must
        // not leak into whatever runs next, in this app or in tests.
        const RestoreCurrentPath restoreCwd;
        std::filesystem::current_path(outputDirStd);
        convert_geotiff::convert(filenameStd, opts, [this](int tx, int ty, int nxt, int nyt) {
            const int done = ty * nxt + tx + 1;
            const int total = nxt * nyt;
            QMetaObject::invokeMethod(this, [this, done, total] {
                progress_->setRange(0, total);
                progress_->setValue(done);
                progress_->setFormat(QString("%1/%2").arg(done).arg(total));
            }, Qt::QueuedConnection);
        });
    });
}

void GeotiffConvertForm::runWorker(std::function<void()> job) {
    if (worker_.joinable()) worker_.join();

    log_->clear();
    progress_->setValue(0);
    progress_->setRange(0, 1);
    setRunning(true);

    worker_ = std::thread([this, job = std::move(job)] {
        QString error;
        bool hasError = false;
        try {
            job();
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
            hasError = true;
        }

        QMetaObject::invokeMethod(this, [this, hasError, error] {
            setRunning(false);
            if (hasError) {
                logLine("ERROR: " + error);
                QMessageBox::critical(this, "Convert", error);
            } else {
                progress_->setValue(progress_->maximum());
                logLine("Conversion complete.");
            }
        }, Qt::QueuedConnection);
    });
}

}  // namespace wrftools
