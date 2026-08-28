#include "wrftools/reproject_form.hpp"

#include "wrftools/error.hpp"
#include "wrftools/reproject.hpp"
#include "wrftools/warp.hpp"
#include "wrftools/wrf_file.hpp"
#include "wrftools/wrf_series.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace wrftools {
namespace {

double parseOptionalDouble(const QLineEdit& field, const QString& label) {
    const auto text = field.text().trimmed();
    if (text.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok) throw UserError((label + " must be a number.").toStdString());
    return value;
}

QString workerExecutablePath() {
    QString name = "wrftools_reproject_worker";
#ifdef _WIN32
    name += ".exe";
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

}  // namespace

ReprojectForm::ReprojectForm(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* inputGroup = new QGroupBox("Input files", this);
    auto* inputForm = new QFormLayout;
    inputSummary_ = new QLineEdit(this);
    inputSummary_->setReadOnly(true);
    browseInputsButton_ = new QPushButton("Browse...", this);
    auto* inputRow = new QWidget(this);
    auto* inputRowLayout = new QHBoxLayout(inputRow);
    inputRowLayout->setContentsMargins(0, 0, 0, 0);
    inputRowLayout->addWidget(inputSummary_);
    inputRowLayout->addWidget(browseInputsButton_);
    inputForm->addRow("wrfout file(s)", inputRow);
    structureLabel_ = new QLabel(this);
    structureLabel_->setWordWrap(true);
    inputForm->addRow("", structureLabel_);
    inputGroup->setLayout(inputForm);
    layout->addWidget(inputGroup);

    // X/Y pixel-size and extent fields are paired on one row each (a small
    // "X"/"Y" sub-label ahead of each field, rather than a separate
    // QFormLayout row per axis) - halves this group's vertical footprint
    // and, combined with the widened tab panel below, keeps every field
    // and its full label on-screen without the horizontal scrollbar a
    // narrower single-column layout forced.
    auto* crsGroup = new QGroupBox("Output CRS and grid", this);
    auto* crsForm = new QFormLayout;
    epsg_ = new QLineEdit("4326", this);
    crsForm->addRow("Target EPSG code", epsg_);

    auto* pixelSizeRow = new QWidget(this);
    auto* pixelSizeRowLayout = new QHBoxLayout(pixelSizeRow);
    pixelSizeRowLayout->setContentsMargins(0, 0, 0, 0);
    pixelSizeX_ = new QLineEdit(this);
    pixelSizeX_->setPlaceholderText("auto");
    pixelSizeY_ = new QLineEdit(this);
    pixelSizeY_->setPlaceholderText("auto");
    pixelSizeRowLayout->addWidget(new QLabel("X", this));
    pixelSizeRowLayout->addWidget(pixelSizeX_);
    pixelSizeRowLayout->addWidget(new QLabel("Y", this));
    pixelSizeRowLayout->addWidget(pixelSizeY_);
    crsForm->addRow("Pixel size", pixelSizeRow);

    auto* extentMinRow = new QWidget(this);
    auto* extentMinRowLayout = new QHBoxLayout(extentMinRow);
    extentMinRowLayout->setContentsMargins(0, 0, 0, 0);
    extentMinX_ = new QLineEdit(this);
    extentMinX_->setPlaceholderText("auto");
    extentMinY_ = new QLineEdit(this);
    extentMinY_->setPlaceholderText("auto");
    extentMinRowLayout->addWidget(new QLabel("X", this));
    extentMinRowLayout->addWidget(extentMinX_);
    extentMinRowLayout->addWidget(new QLabel("Y", this));
    extentMinRowLayout->addWidget(extentMinY_);
    crsForm->addRow("Extent min", extentMinRow);

    auto* extentMaxRow = new QWidget(this);
    auto* extentMaxRowLayout = new QHBoxLayout(extentMaxRow);
    extentMaxRowLayout->setContentsMargins(0, 0, 0, 0);
    extentMaxX_ = new QLineEdit(this);
    extentMaxX_->setPlaceholderText("auto");
    extentMaxY_ = new QLineEdit(this);
    extentMaxY_->setPlaceholderText("auto");
    extentMaxRowLayout->addWidget(new QLabel("X", this));
    extentMaxRowLayout->addWidget(extentMaxX_);
    extentMaxRowLayout->addWidget(new QLabel("Y", this));
    extentMaxRowLayout->addWidget(extentMaxY_);
    crsForm->addRow("Extent max", extentMaxRow);

    crsGroup->setLayout(crsForm);
    layout->addWidget(crsGroup);

    auto* variablesGroup = new QGroupBox("Variables", this);
    auto* variablesLayout = new QVBoxLayout;
    variableFilter_ = new QLineEdit(this);
    variableFilter_->setPlaceholderText("Filter...");
    variablesLayout->addWidget(variableFilter_);
    variables_ = new QListWidget(this);
    variablesLayout->addWidget(variables_);
    auto* selectRow = new QWidget(this);
    auto* selectRowLayout = new QHBoxLayout(selectRow);
    selectRowLayout->setContentsMargins(0, 0, 0, 0);
    selectAllButton_ = new QPushButton("All", this);
    selectNoneButton_ = new QPushButton("None", this);
    selectRowLayout->addWidget(selectAllButton_, 1);
    selectRowLayout->addWidget(selectNoneButton_, 1);
    variablesLayout->addWidget(selectRow);
    variablesGroup->setLayout(variablesLayout);
    layout->addWidget(variablesGroup);

    auto* optionsGroup = new QGroupBox("Options", this);
    auto* optionsForm = new QFormLayout;
    resampling_ = new QComboBox(this);
    resampling_->addItems({"Bilinear", "Nearest", "Average", "Mode"});
    optionsForm->addRow("Resampling", resampling_);
    nearestForCategorical_ = new QCheckBox("Nearest-neighbour for categorical variables", this);
    nearestForCategorical_->setChecked(true);
    optionsForm->addRow("", nearestForCategorical_);
    mergeSeries_ = new QCheckBox("Merge series into one file", this);
    mergeSeries_->setChecked(true);
    optionsForm->addRow("", mergeSeries_);
    outputDirectory_ = new QLineEdit(this);
    outputDirectory_->setReadOnly(true);
    browseOutputButton_ = new QPushButton("Browse...", this);
    auto* outputRow = new QWidget(this);
    auto* outputRowLayout = new QHBoxLayout(outputRow);
    outputRowLayout->setContentsMargins(0, 0, 0, 0);
    outputRowLayout->addWidget(outputDirectory_);
    outputRowLayout->addWidget(browseOutputButton_);
    optionsForm->addRow("Output directory", outputRow);
    optionsGroup->setLayout(optionsForm);
    layout->addWidget(optionsGroup);

    auto* runRow = new QWidget(this);
    auto* runRowLayout = new QHBoxLayout(runRow);
    runRowLayout->setContentsMargins(0, 0, 0, 0);
    runButton_ = new QPushButton("Run", this);
    cancelButton_ = new QPushButton("Cancel", this);
    cancelButton_->setEnabled(false);
    runRowLayout->addWidget(runButton_, 1);
    runRowLayout->addWidget(cancelButton_, 1);
    layout->addWidget(runRow);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1);
    progress_->setValue(0);
    layout->addWidget(progress_);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMinimumHeight(120);
    layout->addWidget(log_);

    auto* resultsGroup = new QGroupBox("Output files", this);
    auto* resultsLayout = new QVBoxLayout;
    results_ = new QListWidget(this);
    resultsLayout->addWidget(results_);
    resultsGroup->setLayout(resultsLayout);
    layout->addWidget(resultsGroup);

    connect(browseInputsButton_, &QPushButton::clicked, this, [this] { browseInputFiles(); });
    connect(browseOutputButton_, &QPushButton::clicked, this, [this] { browseOutputDirectory(); });
    connect(runButton_, &QPushButton::clicked, this, [this] { startReprojectFromSignal(); });
    connect(cancelButton_, &QPushButton::clicked, this, [this] { cancel(); });
    connect(variableFilter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int i = 0; i < variables_->count(); ++i) {
            auto* item = variables_->item(i);
            item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
        }
    });
    connect(selectAllButton_, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < variables_->count(); ++i)
            if (!variables_->item(i)->isHidden()) variables_->item(i)->setCheckState(Qt::Checked);
    });
    connect(selectNoneButton_, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < variables_->count(); ++i)
            if (!variables_->item(i)->isHidden()) variables_->item(i)->setCheckState(Qt::Unchecked);
    });
}

ReprojectForm::~ReprojectForm() {
    if (worker_ && worker_->state() != QProcess::NotRunning) {
        worker_->kill();
        worker_->waitForFinished(1000);
    }
}

void ReprojectForm::setInputPaths(const QStringList& paths) {
    inputPaths_ = paths;
    inputSummary_->setText(paths.join("; "));
    refreshVariables();
}

void ReprojectForm::setOutputDirectory(const QString& path) { outputDirectory_->setText(path); }

void ReprojectForm::browseInputFiles() {
    const auto paths = QFileDialog::getOpenFileNames(this, "Choose wrfout file(s)", {}, "WRF files (*.nc *)");
    if (!paths.isEmpty()) setInputPaths(paths);
}

void ReprojectForm::browseOutputDirectory() {
    const auto path = QFileDialog::getExistingDirectory(this, "Choose an output directory");
    if (!path.isEmpty()) setOutputDirectory(path);
}

void ReprojectForm::refreshVariables() {
    std::map<QString, Qt::CheckState> previousChecks;
    for (int i = 0; i < variables_->count(); ++i) previousChecks[variables_->item(i)->data(Qt::UserRole).toString()] = variables_->item(i)->checkState();
    variables_->clear();
    structureLabel_->clear();

    if (inputPaths_.isEmpty()) return;
    std::vector<std::filesystem::path> paths;
    for (const auto& path : inputPaths_) paths.emplace_back(path.toStdString());

    try {
        const auto grouped = groupWrfPaths(paths);
        if (!grouped.groups.empty())
            structureLabel_->setText(QString("1 series, %1 file(s)").arg(grouped.groups.front().size()));
        else if (paths.size() > 1)
            structureLabel_->setText(QString("%1 unrelated file(s) - not a series").arg(paths.size()));

        WrfFile file(paths.front());
        for (const auto& variable : file.variables()) {
            const auto name = QString::fromStdString(variable.name);
            QString label = name;
            if (!variable.description.empty()) label += " — " + QString::fromStdString(variable.description);
            if (!variable.units.empty()) label += " [" + QString::fromStdString(variable.units) + "]";
            if (variable.levelCount > 1) label += QString(" — 3D, %1 levels").arg(variable.levelCount);
            auto* item = new QListWidgetItem(label, variables_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setData(Qt::UserRole, name);
            const auto previous = previousChecks.find(name);
            item->setCheckState(previous != previousChecks.end() ? previous->second : Qt::Unchecked);
        }
    } catch (const std::exception& error) {
        logLine("Could not read input file(s): " + QString::fromUtf8(error.what()));
    }
}

void ReprojectForm::setRunning(bool running) {
    running_ = running;
    runButton_->setEnabled(!running);
    cancelButton_->setEnabled(running);
    browseInputsButton_->setEnabled(!running);
    browseOutputButton_->setEnabled(!running);
    variables_->setEnabled(!running);
    resampling_->setEnabled(!running);
    mergeSeries_->setEnabled(!running);
}

void ReprojectForm::logLine(const QString& line) { log_->appendPlainText(line); }

void ReprojectForm::cancel() {
    if (worker_ && worker_->state() != QProcess::NotRunning) {
        logLine("Cancelling...");
        worker_->kill();
    }
}

void ReprojectForm::startReprojectFromSignal() {
    try {
        runReproject();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Reproject", QString::fromUtf8(error.what()));
    }
}

void ReprojectForm::runReproject() {
    if (running_) return;
    if (inputPaths_.isEmpty()) throw UserError("Please choose at least one wrfout file.");

    std::vector<QString> selectedVariables;
    for (int i = 0; i < variables_->count(); ++i)
        if (variables_->item(i)->checkState() == Qt::Checked) selectedVariables.push_back(variables_->item(i)->data(Qt::UserRole).toString());
    if (selectedVariables.empty()) throw UserError("Please select at least one variable.");

    bool epsgOk = false;
    const int epsg = epsg_->text().toInt(&epsgOk);
    if (!epsgOk || epsg <= 0) throw UserError("Target EPSG code must be a positive integer.");

    const double pixelSizeX = parseOptionalDouble(*pixelSizeX_, "Pixel size X");
    const double pixelSizeY = parseOptionalDouble(*pixelSizeY_, "Pixel size Y");
    if (!std::isnan(pixelSizeX) && pixelSizeX <= 0) throw UserError("Pixel size X must be a positive number.");
    if (!std::isnan(pixelSizeY) && pixelSizeY <= 0) throw UserError("Pixel size Y must be a positive number.");

    const double extentMinX = parseOptionalDouble(*extentMinX_, "Extent min X");
    const double extentMinY = parseOptionalDouble(*extentMinY_, "Extent min Y");
    const double extentMaxX = parseOptionalDouble(*extentMaxX_, "Extent max X");
    const double extentMaxY = parseOptionalDouble(*extentMaxY_, "Extent max Y");
    const int extentFieldsSet =
        (!std::isnan(extentMinX)) + (!std::isnan(extentMinY)) + (!std::isnan(extentMaxX)) + (!std::isnan(extentMaxY));
    if (extentFieldsSet != 0 && extentFieldsSet != 4) throw UserError("The output extent must be given as all four of min X/Y and max X/Y, or none.");

    if (outputDirectory_->text().isEmpty()) throw UserError("Please choose an output directory.");
    if (!QDir(outputDirectory_->text()).exists()) throw UserError("Output directory does not exist: " + outputDirectory_->text().toStdString());

    const auto workerPath = workerExecutablePath();
    if (!QFile::exists(workerPath)) throw UserError("Reproject worker executable not found: " + workerPath.toStdString());

    QJsonObject job;
    QJsonArray inputs;
    for (const auto& path : inputPaths_) inputs.append(path);
    job["inputs"] = inputs;
    job["targetEpsg"] = epsg;
    QJsonArray variablesArray;
    for (const auto& name : selectedVariables) variablesArray.append(name);
    job["variables"] = variablesArray;
    job["seriesMode"] = mergeSeries_->isChecked() ? "merge" : "perfile";
    job["outputDirectory"] = outputDirectory_->text();
    job["resampling"] = resampling_->currentText().toLower();
    job["nearestForCategorical"] = nearestForCategorical_->isChecked();
    QJsonObject grid;
    if (!std::isnan(pixelSizeX)) grid["pixelSizeX"] = pixelSizeX;
    if (!std::isnan(pixelSizeY)) grid["pixelSizeY"] = pixelSizeY;
    if (extentFieldsSet == 4) {
        QJsonObject extent;
        extent["minX"] = extentMinX;
        extent["minY"] = extentMinY;
        extent["maxX"] = extentMaxX;
        extent["maxY"] = extentMaxY;
        grid["extent"] = extent;
    }
    job["grid"] = grid;

    const auto jobFile = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                              .filePath(QString("wrftools-reproject-job-%1.json").arg(QCoreApplication::applicationPid()));
    QFile file(jobFile);
    if (!file.open(QIODevice::WriteOnly)) throw UserError("Could not write job file: " + jobFile.toStdString());
    file.write(QJsonDocument(job).toJson());
    file.close();
    jobFilePath_ = jobFile;

    log_->clear();
    results_->clear();
    progress_->setRange(0, 0);  // busy, until the worker's first PROGRESS line gives a real total
    progress_->setValue(0);
    setRunning(true);
    logLine("Starting reprojection...");

    if (!worker_) {
        worker_ = new QProcess(this);
        connect(worker_, &QProcess::readyReadStandardOutput, this, [this] { handleWorkerOutput(); });
        connect(worker_, &QProcess::readyReadStandardError, this, [this] { handleWorkerOutput(); });
        connect(worker_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) { handleWorkerFinished(exitCode, static_cast<int>(status)); });
    }
    worker_->start(workerPath, {jobFilePath_});
}

void ReprojectForm::handleWorkerOutput() {
    if (!worker_) return;
    worker_->setReadChannel(QProcess::StandardOutput);
    while (worker_->canReadLine()) {
        const auto line = QString::fromUtf8(worker_->readLine()).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith("PROGRESS ")) {
            const auto rest = line.mid(9);
            const auto firstSpace = rest.indexOf(' ');
            const auto secondSpace = rest.indexOf(' ', firstSpace + 1);
            if (firstSpace > 0 && secondSpace > firstSpace) {
                const auto completed = rest.left(firstSpace).toULongLong();
                const auto total = rest.mid(firstSpace + 1, secondSpace - firstSpace - 1).toULongLong();
                const auto message = rest.mid(secondSpace + 1);
                if (total > 0) {
                    if (progress_->maximum() != static_cast<int>(std::min<qulonglong>(total, INT_MAX))) progress_->setRange(0, static_cast<int>(std::min<qulonglong>(total, INT_MAX)));
                    progress_->setValue(static_cast<int>(std::min<qulonglong>(completed, INT_MAX)));
                }
                if (!message.isEmpty()) logLine(message);
            }
        } else if (line.startsWith("DONE ")) {
            new QListWidgetItem(line.mid(5), results_);
        } else if (line.startsWith("ERROR ")) {
            logLine("ERROR: " + line.mid(6));
        } else {
            logLine(line);
        }
    }
    const auto errorOutput = worker_->readAllStandardError();
    if (!errorOutput.isEmpty()) logLine(QString::fromUtf8(errorOutput).trimmed());
}

void ReprojectForm::handleWorkerFinished(int exitCode, int exitStatus) {
    handleWorkerOutput();
    setRunning(false);
    QFile::remove(jobFilePath_);
    if (exitStatus != QProcess::NormalExit) {
        logLine("Reprojection was cancelled or crashed.");
    } else if (exitCode != 0) {
        logLine("Reprojection failed.");
        QMessageBox::warning(this, "Reproject", "Reprojection failed - see the log for details.");
    } else {
        progress_->setValue(progress_->maximum());
        logLine("Reprojection complete.");
    }
}

}  // namespace wrftools
