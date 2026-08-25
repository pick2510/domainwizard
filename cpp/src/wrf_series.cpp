#include "wrftools/wrf_series.hpp"

#include <algorithm>
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

WrfFileSeries::WrfFileSeries(std::vector<std::filesystem::path> paths) : paths_(std::move(paths)) {
    if (paths_.size() < 2) throw UserError("A WRF file series needs at least two files.");
    std::sort(paths_.begin(), paths_.end(), [](const auto& left, const auto& right) { return parseWrfFilename(left)->validTime < parseWrfFilename(right)->validTime; });
    for (const auto& path : paths_) {
        const auto parsed = parseWrfFilename(path);
        if (!parsed) throw UserError("A series file has no recognized WRF timestamp: " + path.string());
        const auto day = std::chrono::floor<std::chrono::days>(parsed->validTime);
        const std::chrono::year_month_day date{day};
        const auto clock = std::chrono::hh_mm_ss{parsed->validTime - day};
        std::ostringstream label;
        label << int(date.year()) << '-' << std::setfill('0') << std::setw(2) << unsigned(date.month()) << '-' << std::setw(2) << unsigned(date.day()) << ' ' << std::setw(2) << clock.hours().count() << ':' << std::setw(2) << clock.minutes().count();
        times_.push_back(label.str());
    }
    fileAt(0);
}

WrfFile& WrfFileSeries::fileAt(std::size_t index) {
    if (const auto found = files_.find(index); found != files_.end()) return *found->second;
    auto opened = std::make_unique<WrfFile>(paths_.at(index));
    if (!files_.empty()) {
        const auto reference = files_.at(0)->size();
        if (opened->size() != reference) throw UserError("WRF series files have incompatible grids.");
    }
    auto [it, inserted] = files_.emplace(index, std::move(opened));
    return *it->second;
}

std::vector<float> WrfFileSeries::read(const std::string& variable, int timeIndex, int levelIndex) {
    if (timeIndex < 0 || timeIndex >= static_cast<int>(paths_.size())) throw UserError("WRF series time index is out of range.");
    return fileAt(static_cast<std::size_t>(timeIndex)).read(variable, 0, levelIndex);
}
}  // namespace wrftools
