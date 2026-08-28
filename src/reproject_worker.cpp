// Headless worker for the Reproject tab. Runs runReproject() in its own,
// single-threaded process - deliberately NOT inside the GUI process, and
// NOT on a background std::thread within it. See reproject_form.hpp/
// lcz_form.hpp for the reproduced deadlock this avoids: netCDF-C/GDAL/HDF5
// gets thread-affined to whichever OS thread first touches it (the GUI's
// main.cpp calls GDALAllRegister() on the GUI thread at startup), and on at
// least one real configuration (Fedora-style non-threadsafe libhdf5),
// touching it again from a second thread afterward deadlocks the whole
// process even with no actual concurrency. A fresh process shares none of
// that state, so the deadlock condition cannot occur, and the GUI process's
// own event loop is never blocked while this runs.
//
// Protocol: argv[1] is a path to a JSON job file (see readJob below,
// written by ReprojectForm to match wrftools::ReprojectOptions). Progress
// is reported as one line per event on stdout, flushed immediately:
//   PROGRESS <completed> <total> <message>
//   DONE <path>      (once per output file actually written)
//   ERROR <message>  (on failure; exit code is then non-zero)
#include "wrftools/reproject.hpp"
#include "wrftools/error.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <iostream>

namespace {

wrftools::ReprojectOptions readJob(const QString& jobPath) {
    QFile file(jobPath);
    if (!file.open(QIODevice::ReadOnly)) throw wrftools::UserError("Could not read job file: " + jobPath.toStdString());
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        throw wrftools::UserError("Invalid job file: " + parseError.errorString().toStdString());
    const auto root = document.object();

    wrftools::ReprojectOptions options;
    for (const auto& value : root.value("inputs").toArray()) options.inputs.emplace_back(value.toString().toStdString());
    options.targetEpsg = root.value("targetEpsg").toInt();
    for (const auto& value : root.value("variables").toArray()) options.variables.push_back(value.toString().toStdString());
    options.seriesMode = root.value("seriesMode").toString() == "perfile" ? wrftools::SeriesMode::PerFile : wrftools::SeriesMode::Merge;
    options.outputDirectory = root.value("outputDirectory").toString().toStdString();
    const auto resampling = root.value("resampling").toString();
    options.resampling = resampling == "nearest"   ? wrftools::ResampleMethod::Nearest
                          : resampling == "average" ? wrftools::ResampleMethod::Average
                          : resampling == "mode"     ? wrftools::ResampleMethod::Mode
                                                      : wrftools::ResampleMethod::Bilinear;
    options.nearestForCategorical = root.value("nearestForCategorical").toBool(true);

    const auto gridObject = root.value("grid").toObject();
    if (gridObject.contains("pixelSizeX")) options.grid.pixelSizeX = gridObject.value("pixelSizeX").toDouble();
    if (gridObject.contains("pixelSizeY")) options.grid.pixelSizeY = gridObject.value("pixelSizeY").toDouble();
    if (gridObject.contains("extent")) {
        const auto extent = gridObject.value("extent").toObject();
        options.grid.extent = wrftools::Bounds2D{extent.value("minX").toDouble(), extent.value("minY").toDouble(), extent.value("maxX").toDouble(),
            extent.value("maxY").toDouble()};
    }
    return options;
}

void emitLine(const std::string& line) {
    std::cout << line << '\n';
    std::cout.flush();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    if (argc < 2) {
        emitLine("ERROR Usage: wrftools_reproject_worker <job.json>");
        return 1;
    }
    try {
        const auto options = readJob(QString::fromLocal8Bit(argv[1]));
        const auto written = wrftools::runReproject(options, [](const wrftools::ReprojectProgress& progress) {
            emitLine("PROGRESS " + std::to_string(progress.completed) + " " + std::to_string(progress.total) + " " + progress.message);
        });
        for (const auto& path : written) emitLine("DONE " + path.string());
        return 0;
    } catch (const std::exception& error) {
        emitLine(std::string("ERROR ") + error.what());
        return 1;
    }
}
