#include "wrftools/lcz_form.hpp"

#include "wrftools/error.hpp"
#include "wrftools/lcz.hpp"
#include "wrftools/netcdf_file.hpp"

#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <QDir>

#include <cmath>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace wrftools {
namespace {

// Parses w2w's own --built-lcz syntax: whitespace-separated integers
// (argparse's nargs='+' with type=int). Throws UserError on anything else,
// including an empty list - a run with zero built LCZ classes has no
// built-up UCP content to compute.
std::vector<int> parseBuiltLcz(const QString& text) {
    std::vector<int> values;
    std::istringstream stream(text.toStdString());
    std::string token;
    while (stream >> token) {
        try {
            values.push_back(std::stoi(token));
        } catch (const std::exception&) {
            throw UserError("Built LCZ classes must be whitespace-separated integers (e.g. \"1 2 3 4 5 6 7 8 9 10\"): \"" + token + "\" is not one.");
        }
    }
    if (values.empty()) throw UserError("Please specify at least one built LCZ class.");
    return values;
}

int parseRequiredInt(const QLineEdit& field, const QString& label) {
    bool ok = false;
    const int value = field.text().toInt(&ok);
    if (!ok) throw UserError((label + " must be an integer.").toStdString());
    return value;
}

double parseRequiredDouble(const QLineEdit& field, const QString& label) {
    bool ok = false;
    const double value = field.text().toDouble(&ok);
    if (!ok) throw UserError((label + " must be a number.").toStdString());
    return value;
}

}  // namespace

LczForm::LczForm(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* filesGroup = new QGroupBox("Files", this);
    auto* filesForm = new QFormLayout;
    lczFile_ = new QLineEdit(this);
    lczFile_->setReadOnly(true);
    browseLczButton_ = new QPushButton("Browse...", this);
    auto* lczRow = new QWidget(this);
    auto* lczRowLayout = new QHBoxLayout(lczRow);
    lczRowLayout->setContentsMargins(0, 0, 0, 0);
    lczRowLayout->addWidget(lczFile_);
    lczRowLayout->addWidget(browseLczButton_);
    filesForm->addRow("LCZ GeoTIFF file", lczRow);

    wrfFile_ = new QLineEdit(this);
    wrfFile_->setReadOnly(true);
    browseWrfButton_ = new QPushButton("Browse...", this);
    auto* wrfRow = new QWidget(this);
    auto* wrfRowLayout = new QHBoxLayout(wrfRow);
    wrfRowLayout->setContentsMargins(0, 0, 0, 0);
    wrfRowLayout->addWidget(wrfFile_);
    wrfRowLayout->addWidget(browseWrfButton_);
    filesForm->addRow("Target geo_em.dXX.nc file", wrfRow);
    filesGroup->setLayout(filesForm);
    layout->addWidget(filesGroup);

    auto* optionsGroup = new QGroupBox("LCZ Options", this);
    auto* optionsForm = new QFormLayout;
    optionsForm->setVerticalSpacing(10);

    wrfVersion_ = new QComboBox(this);
    for (const auto& option : wrfVersionOptions()) wrfVersion_->addItem(QString::fromStdString(option.name));
    optionsForm->addRow("WRF version", wrfVersion_);

    builtLcz_ = new QLineEdit("1 2 3 4 5 6 7 8 9 10", this);
    optionsForm->addRow("Built LCZ classes", builtLcz_);
    lczBand_ = new QLineEdit("0", this);
    optionsForm->addRow("LCZ GeoTIFF band", lczBand_);
    frcThreshold_ = new QLineEdit("0.2", this);
    optionsForm->addRow("FRC_URB2D threshold", frcThreshold_);
    npixNlc_ = new QLineEdit("45", this);
    optionsForm->addRow("Natural-land sampling pixels", npixNlc_);
    npixArea_ = new QLineEdit(this);
    npixArea_->setPlaceholderText(QString("Default: NPIX_NLC²"));
    optionsForm->addRow("Sampling search area (pixels)", npixArea_);

    customUcpTable_ = new QLineEdit(this);
    customUcpTable_->setReadOnly(true);
    customUcpTable_->setPlaceholderText("Default: bundled LCZ_UCP_lookup.csv");
    browseCustomUcpButton_ = new QPushButton("Browse...", this);
    auto* ucpRow = new QWidget(this);
    auto* ucpRowLayout = new QHBoxLayout(ucpRow);
    ucpRowLayout->setContentsMargins(0, 0, 0, 0);
    ucpRowLayout->addWidget(customUcpTable_);
    ucpRowLayout->addWidget(browseCustomUcpButton_);
    optionsForm->addRow("Custom LCZ UCP table", ucpRow);

    optionsGroup->setLayout(optionsForm);
    layout->addWidget(optionsGroup);

    runButton_ = new QPushButton("Run", this);
    layout->addWidget(runButton_);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1);
    progress_->setValue(0);
    layout->addWidget(progress_);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMinimumHeight(120);
    layout->addWidget(log_);

    auto* resultsGroup = new QGroupBox("Sanity checks", this);
    auto* resultsLayout = new QVBoxLayout;
    results_ = new QListWidget(this);
    resultsLayout->addWidget(results_);
    nbuiMaxLabel_ = new QLabel(this);
    resultsLayout->addWidget(nbuiMaxLabel_);
    resultsGroup->setLayout(resultsLayout);
    layout->addWidget(resultsGroup);

    connect(browseLczButton_, &QPushButton::clicked, this, [this] { browseLczFile(); });
    connect(browseWrfButton_, &QPushButton::clicked, this, [this] { browseWrfFile(); });
    connect(browseCustomUcpButton_, &QPushButton::clicked, this, [this] { browseCustomUcpTable(); });
    connect(runButton_, &QPushButton::clicked, this, [this] { startLczFromSignal(); });
}

LczForm::~LczForm() = default;

void LczForm::setLczFile(const QString& path) { lczFile_->setText(path); }
void LczForm::setWrfFile(const QString& path) { wrfFile_->setText(path); }
void LczForm::setCustomUcpTable(const QString& path) { customUcpTable_->setText(path); }

void LczForm::browseLczFile() {
    const auto path = QFileDialog::getOpenFileName(this, "Choose an LCZ GeoTIFF file", {}, "GeoTIFF Files (*.tif *.tiff)");
    if (!path.isEmpty()) setLczFile(path);
}

void LczForm::browseWrfFile() {
    const auto path = QFileDialog::getOpenFileName(this, "Choose the target geo_em.dXX.nc file", {}, "WRF geo_em files (*.nc)");
    if (!path.isEmpty()) setWrfFile(path);
}

void LczForm::browseCustomUcpTable() {
    const auto path = QFileDialog::getOpenFileName(this, "Choose a custom LCZ UCP table", {}, "CSV Files (*.csv)");
    if (!path.isEmpty()) setCustomUcpTable(path);
}

void LczForm::setRunning(bool running) {
    running_ = running;
    runButton_->setEnabled(!running);
    browseLczButton_->setEnabled(!running);
    browseWrfButton_->setEnabled(!running);
    browseCustomUcpButton_->setEnabled(!running);
    wrfVersion_->setEnabled(!running);
}

void LczForm::logLine(const QString& line) { log_->appendPlainText(line); }

void LczForm::startLczFromSignal() {
    try {
        runLcz();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "LCZ", QString::fromUtf8(error.what()));
    }
}

void LczForm::runLcz() {
    if (running_) return;

    const auto lczFile = lczFile_->text();
    const auto wrfFile = wrfFile_->text();
    if (lczFile.isEmpty()) throw UserError("Please choose an LCZ GeoTIFF file.");
    if (wrfFile.isEmpty()) throw UserError("Please choose the target geo_em.dXX.nc file.");
    if (!std::filesystem::exists(lczFile.toStdString())) throw UserError("LCZ GeoTIFF file does not exist: " + lczFile.toStdString());
    if (!std::filesystem::exists(wrfFile.toStdString())) throw UserError("Target geo_em file does not exist: " + wrfFile.toStdString());

    const auto wrfFileStd = std::filesystem::path(wrfFile.toStdString());
    const std::string wrfFileString = wrfFileStd.string();
    if (wrfFileString.size() < 3 || wrfFileString.substr(wrfFileString.size() - 3) != ".nc")
        throw UserError("Target file must end in .nc: " + wrfFileString);

    const auto builtLcz = parseBuiltLcz(builtLcz_->text());
    const int lczBand = parseRequiredInt(*lczBand_, "LCZ GeoTIFF band");
    const double frcThreshold = parseRequiredDouble(*frcThreshold_, "FRC_URB2D threshold");
    const int npixNlc = parseRequiredInt(*npixNlc_, "Natural-land sampling pixels");
    if (npixNlc <= 0) throw UserError("Natural-land sampling pixels must be > 0.");
    std::optional<int> npixArea;
    if (!npixArea_->text().trimmed().isEmpty()) {
        const int value = parseRequiredInt(*npixArea_, "Sampling search area");
        if (value <= 0) throw UserError("Sampling search area must be > 0.");
        npixArea = value;
    }

    const int versionIndex = wrfVersion_->currentIndex();
    if (versionIndex < 0 || static_cast<std::size_t>(versionIndex) >= wrfVersionOptions().size()) throw UserError("Please choose a WRF version.");
    const auto wrfVersion = wrfVersionOptions()[static_cast<std::size_t>(versionIndex)].info;

    const bool usingCustomUcpTable = !customUcpTable_->text().isEmpty();
    const auto ucpTable = usingCustomUcpTable ? loadUcpTable(customUcpTable_->text().toStdString()) : defaultUcpTable();
    if (usingCustomUcpTable) checkCustomUcpTableIntegrity(ucpTable);

    log_->clear();
    results_->clear();
    nbuiMaxLabel_->clear();
    progress_->setRange(0, 0);  // busy indicator - the pipeline has no natural per-step unit count
    setRunning(true);
    // See the class comment in lcz_form.hpp for why this whole pipeline
    // runs right here on the GUI thread rather than a background
    // std::thread: on at least one real configuration, netCDF-C/GDAL used
    // from a second OS thread after the GUI thread has already touched
    // them deadlocks, regardless of handle lifetime. Always reset
    // running_ on the way out, including via an exception - RunningGuard's
    // destructor runs during stack unwinding just as much as on a normal
    // return.
    struct RunningGuard {
        LczForm* form;
        ~RunningGuard() { form->setRunning(false); }
    } runningGuard{this};

    try {
        logLine("Check LCZ integrity...");
        QCoreApplication::processEvents();
        auto clean = [&] {
            const auto wrfNetcdf = NetcdfFile::open(wrfFileStd, NetcdfFile::Mode::ReadOnly);
            return checkLczIntegrity(lczFile.toStdString(), lczBand, wrfNetcdf);
        }();
        logLine("Check LCZ integrity: OK.");

        const auto stem = wrfFileString.substr(0, wrfFileString.size() - 3);
        const auto dstNuFile = stem + "_NoUrban.nc";
        const auto dstLczParamsFile = stem + "_LCZ_params.nc";
        const auto dstLczExtentFile = stem + "_LCZ_extent.nc";

        logLine("Replace WRF urban land use with surrounding natural land cover...");
        QCoreApplication::processEvents();
        removeUrban(wrfFileString, dstNuFile, npixNlc, npixArea);

        logLine("Create LCZ-based geo_em file...");
        QCoreApplication::processEvents();
        const LczParamsInputs inputs{dstNuFile, wrfFileString, clean, builtLcz, ucpTable, wrfVersion, frcThreshold};
        const int nbuiMax = createLczParamsFile(inputs, dstLczParamsFile);

        logLine("Create LCZ-based urban extent geo_em file...");
        QCoreApplication::processEvents();
        createLczExtentFile(dstLczParamsFile, wrfFileString, dstLczExtentFile);

        logLine("Expanding land categories of parent domain(s)...");
        QCoreApplication::processEvents();
        const auto parentMessages = expandLandCatParents(wrfFileString, wrfVersion);
        for (const auto& message : parentMessages) logLine(QString::fromStdString(message));

        logLine("Running sanity checks...");
        QCoreApplication::processEvents();
        // No *_clean.tif is ever written to disk by this port's
        // checkLczIntegrity (the cleaned raster stays in memory - see
        // lcz.hpp) - pass a path that can never exist so
        // checksAndCleaning's cleanup step is a harmless no-op.
        const ChecksAndCleaningInputs checksInputs{"", wrfFileString, dstNuFile, dstLczExtentFile, dstLczParamsFile};
        const auto checkResults = checksAndCleaning(checksInputs, ucpTable);

        progress_->setRange(0, 1);
        progress_->setValue(1);
        for (const auto& result : checkResults) {
            auto* item = new QListWidgetItem(QString::fromStdString(result.name + ": " + result.message), results_);
            item->setForeground(result.status == CheckStatus::Ok ? QColor(Qt::darkGreen) : QColor(Qt::darkYellow));
        }
        nbuiMaxLabel_->setText(QString("Set nbui_max to %1 during compilation, in order to optimize memory storage.").arg(nbuiMax));
        logLine("LCZ processing complete.");
    } catch (const std::exception& e) {
        progress_->setRange(0, 1);
        progress_->setValue(0);
        logLine("ERROR: " + QString::fromUtf8(e.what()));
        throw;
    }
}

}  // namespace wrftools
