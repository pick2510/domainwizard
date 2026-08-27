#include "wrftools/lcz_form.hpp"

#include "wrftools/error.hpp"
#include "wrftools/lcz.hpp"
#include "wrftools/netcdf_file.hpp"

#include <QColor>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMetaObject>
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

LczForm::~LczForm() {
    if (worker_.joinable()) worker_.join();
}

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

    // check_lcz_integrity runs synchronously here, on the GUI thread,
    // before the Run button even disables - matching w2w.py's own main()
    // sequencing (it's literally the first pipeline step) while surfacing
    // a domain-coverage/band mistake as an immediate UserError rather than
    // only discovering it after the background job has already started.
    // This does mean a large LCZ raster's reprojection briefly blocks the
    // UI; PORT_W2W.MD documents this as a deliberate simplification rather
    // than adding a second background pre-check pass for one warp call.
    const auto wrfNetcdf = NetcdfFile::open(wrfFileStd, NetcdfFile::Mode::ReadOnly);
    auto clean = checkLczIntegrity(lczFile.toStdString(), lczBand, wrfNetcdf);

    log_->clear();
    results_->clear();
    nbuiMaxLabel_->clear();
    progress_->setRange(0, 0);  // busy indicator - the pipeline has no natural per-step unit count
    setRunning(true);
    logLine("Check LCZ integrity: OK.");

    worker_ = std::thread([this, wrfFileString, builtLcz, ucpTable, wrfVersion, frcThreshold, npixNlc, npixArea, clean = std::move(clean)]() mutable {
        QString error;
        bool hasError = false;
        std::vector<std::string> parentMessages;
        std::vector<CheckResult> checkResults;
        int nbuiMax = 0;
        try {
            const auto stem = wrfFileString.substr(0, wrfFileString.size() - 3);
            const auto dstNuFile = stem + "_NoUrban.nc";
            const auto dstLczParamsFile = stem + "_LCZ_params.nc";
            const auto dstLczExtentFile = stem + "_LCZ_extent.nc";

            QMetaObject::invokeMethod(this, [this] { logLine("Replace WRF urban land use with surrounding natural land cover..."); }, Qt::QueuedConnection);
            removeUrban(wrfFileString, dstNuFile, npixNlc, npixArea);

            QMetaObject::invokeMethod(this, [this] { logLine("Create LCZ-based geo_em file..."); }, Qt::QueuedConnection);
            const LczParamsInputs inputs{dstNuFile, wrfFileString, clean, builtLcz, ucpTable, wrfVersion, frcThreshold};
            nbuiMax = createLczParamsFile(inputs, dstLczParamsFile);

            QMetaObject::invokeMethod(
                this, [this] { logLine("Create LCZ-based urban extent geo_em file..."); }, Qt::QueuedConnection);
            createLczExtentFile(dstLczParamsFile, wrfFileString, dstLczExtentFile);

            QMetaObject::invokeMethod(
                this, [this] { logLine("Expanding land categories of parent domain(s)..."); }, Qt::QueuedConnection);
            parentMessages = expandLandCatParents(wrfFileString, wrfVersion);

            QMetaObject::invokeMethod(this, [this] { logLine("Running sanity checks..."); }, Qt::QueuedConnection);
            // No *_clean.tif is ever written to disk by this port's
            // checkLczIntegrity (the cleaned raster stays in memory - see
            // lcz.hpp) - pass a path that can never exist so
            // checksAndCleaning's cleanup step is a harmless no-op.
            const ChecksAndCleaningInputs checksInputs{"", wrfFileString, dstNuFile, dstLczExtentFile, dstLczParamsFile};
            checkResults = checksAndCleaning(checksInputs, ucpTable);
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
            hasError = true;
        }

        QMetaObject::invokeMethod(
            this,
            [this, hasError, error, parentMessages, checkResults, nbuiMax] {
                setRunning(false);
                progress_->setRange(0, 1);
                if (hasError) {
                    progress_->setValue(0);
                    logLine("ERROR: " + error);
                    QMessageBox::critical(this, "LCZ", error);
                    return;
                }
                progress_->setValue(1);
                for (const auto& message : parentMessages) logLine(QString::fromStdString(message));
                for (const auto& result : checkResults) {
                    auto* item = new QListWidgetItem(QString::fromStdString(result.name + ": " + result.message), results_);
                    item->setForeground(result.status == CheckStatus::Ok ? QColor(Qt::darkGreen) : QColor(Qt::darkYellow));
                }
                nbuiMaxLabel_->setText(QString("Set nbui_max to %1 during compilation, in order to optimize memory storage.").arg(nbuiMax));
                logLine("LCZ processing complete.");
            },
            Qt::QueuedConnection);
    });
}

}  // namespace wrftools
