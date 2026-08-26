#include "wrftools/wrf_series.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <iomanip>
#include <sstream>

#include "wrftools/error.hpp"

namespace wrftools {
namespace {
const std::regex kNamePattern(
    R"(^(wrfout|wrfrst|met_em)[._]d([0-9]{2})[._]([0-9]{4})-([0-9]{2})-([0-9]{2})[_.]([0-9]{2})[:_]([0-9]{2})[:_]([0-9]{2})(?:\.nc)?$)");
}

std::optional<ParsedWrfName> parseWrfFilename(const std::filesystem::path& path) {
    std::smatch match;
    const auto name = path.filename().string();
    if (!std::regex_match(name, match, kNamePattern)) return std::nullopt;
    using namespace std::chrono;
    const year_month_day date{year{std::stoi(match[3])}, month{static_cast<unsigned>(std::stoi(match[4]))}, std::chrono::day{static_cast<unsigned>(std::stoi(match[5]))}};
    if (!date.ok()) return std::nullopt;
    const auto hour = std::stoi(match[6]), minute = std::stoi(match[7]), second = std::stoi(match[8]);
    if (hour > 23 || minute > 59 || second > 59) return std::nullopt;
    return ParsedWrfName{match[1], match[2], sys_days{date} + hours{hour} + minutes{minute} + seconds{second}};
}

GroupedPaths groupWrfPaths(const std::vector<std::filesystem::path>& paths) {
    std::map<std::pair<std::string, std::string>, std::vector<std::filesystem::path>> candidates;
    GroupedPaths result;
    for (const auto& path : paths) {
        if (auto parsed = parseWrfFilename(path)) candidates[{parsed->kind, parsed->domain}].push_back(path);
        else result.singles.push_back(path);
    }
    for (auto& [key, group] : candidates) {
        if (group.size() == 1) { result.singles.push_back(group.front()); continue; }
        std::sort(group.begin(), group.end(), [](const auto& left, const auto& right) {
            return parseWrfFilename(left)->validTime < parseWrfFilename(right)->validTime ||
                   (parseWrfFilename(left)->validTime == parseWrfFilename(right)->validTime && left < right);
        });
        result.groups.push_back(std::move(group));
    }
    return result;
}

namespace {
constexpr double kGeotransformTolerance = 1e-6;

std::string formatTimestamp(const std::chrono::sys_seconds& validTime) {
    const auto day = std::chrono::floor<std::chrono::days>(validTime);
    const std::chrono::year_month_day date{day};
    const auto clock = std::chrono::hh_mm_ss{validTime - day};
    std::ostringstream label;
    label << int(date.year()) << '-' << std::setfill('0') << std::setw(2) << unsigned(date.month()) << '-'
          << std::setw(2) << unsigned(date.day()) << ' ' << std::setw(2) << clock.hours().count() << ':'
          << std::setw(2) << clock.minutes().count();
    return label.str();
}

int fileTimeCount(const WrfFile& file) {
    int maxCount = 1;
    for (const auto& variable : file.variables()) maxCount = std::max(maxCount, variable.timeCount);
    return maxCount;
}

void checkSameGrid(const WrfFile& first, const WrfFile& other) {
    if (other.projectionWkt() != first.projectionWkt() || other.size() != first.size())
        throw UserError("WRF series files have incompatible grids.");
    const auto& a = first.geotransform();
    const auto& b = other.geotransform();
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > kGeotransformTolerance) throw UserError("WRF series files have incompatible grids.");
}
}  // namespace

WrfFileSeries::WrfFileSeries(std::vector<std::filesystem::path> paths) : paths_(std::move(paths)) {
    if (paths_.size() < 2) throw UserError("A WRF file series needs at least two files.");
    auto first = std::make_unique<WrfFile>(paths_.front());
    variables_ = first->variables();

    std::vector<std::optional<ParsedWrfName>> parsed;
    parsed.reserve(paths_.size());
    for (const auto& path : paths_) parsed.push_back(parseWrfFilename(path));
    const bool allParsed = std::all_of(parsed.begin(), parsed.end(), [](const auto& p) { return p.has_value(); });

    if (fileTimeCount(*first) == 1 && allParsed) {
        // Fast path: every file's valid time comes straight from its
        // filename, and the first file has exactly one internal timestep -
        // no need to open anything else. See wrfseries.py's module
        // docstring for why this matters for large series.
        for (const auto& p : parsed) { times_.push_back(formatTimestamp(p->validTime)); }
        for (std::size_t i = 0; i < paths_.size(); ++i) timeMap_.emplace_back(i, 0);
        files_.emplace(0, std::move(first));
    } else {
        // Eager fallback: some file's timestep count can't be inferred from
        // its name alone, so every file must be opened up front to build a
        // real time map, and .variables() becomes the true cross-file
        // intersection.
        files_.emplace(0, std::move(first));
        for (std::size_t i = 1; i < paths_.size(); ++i) fileAt(i);

        std::vector<std::string> commonNames;
        for (const auto& variable : files_.at(0)->variables()) {
            bool inAll = true;
            for (std::size_t i = 1; i < paths_.size() && inAll; ++i) {
                const auto& others = files_.at(i)->variables();
                inAll = std::any_of(others.begin(), others.end(), [&](const auto& v) { return v.name == variable.name; });
            }
            if (inAll) commonNames.push_back(variable.name);
        }
        variables_.clear();
        for (const auto& variable : files_.at(0)->variables())
            if (std::find(commonNames.begin(), commonNames.end(), variable.name) != commonNames.end()) variables_.push_back(variable);

        std::size_t total = 0;
        for (std::size_t i = 0; i < paths_.size(); ++i) total += static_cast<std::size_t>(fileTimeCount(*files_.at(i)));
        std::size_t step = 0;
        for (std::size_t fileIndex = 0; fileIndex < paths_.size(); ++fileIndex) {
            const int stepsInFile = fileTimeCount(*files_.at(fileIndex));
            for (int local = 0; local < stepsInFile; ++local) {
                ++step;
                std::ostringstream label;
                label << "File " << (fileIndex + 1) << ", Step " << (local + 1) << " of " << stepsInFile
                      << " (" << step << " of " << total << ")";
                times_.push_back(label.str());
                timeMap_.emplace_back(fileIndex, local);
            }
        }
    }
}

std::string WrfFileSeries::name() const {
    const auto firstParsed = parseWrfFilename(paths_.front());
    const std::string prefix = firstParsed ? (firstParsed->kind + "_d" + firstParsed->domain) : files_.at(0)->path().filename().string();
    const auto lastParsed = parseWrfFilename(paths_.back());
    if (firstParsed && lastParsed) {
        return prefix + " (" + std::to_string(paths_.size()) + " files, " + formatTimestamp(firstParsed->validTime) +
               " - " + formatTimestamp(lastParsed->validTime) + ")";
    }
    return prefix + " (" + std::to_string(paths_.size()) + " files)";
}

WrfFile& WrfFileSeries::fileAt(std::size_t index) {
    if (const auto found = files_.find(index); found != files_.end()) return *found->second;
    auto opened = std::make_unique<WrfFile>(paths_.at(index));
    checkSameGrid(*files_.at(0), *opened);
    auto [it, inserted] = files_.emplace(index, std::move(opened));
    return *it->second;
}

std::vector<float> WrfFileSeries::read(const std::string& variable, int timeIndex, int levelIndex) {
    if (timeIndex < 0 || timeIndex >= static_cast<int>(timeMap_.size())) throw UserError("WRF series time index is out of range.");
    const auto [fileIndex, localTimeIndex] = timeMap_.at(static_cast<std::size_t>(timeIndex));
    return fileAt(fileIndex).read(variable, localTimeIndex, levelIndex);
}
}  // namespace wrftools
