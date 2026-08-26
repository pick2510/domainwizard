#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <utility>

#include "wrftools/wrf_file.hpp"

namespace wrftools {

struct ParsedWrfName {
    std::string kind;
    std::string domain;
    std::chrono::sys_seconds validTime;
};

[[nodiscard]] std::optional<ParsedWrfName> parseWrfFilename(const std::filesystem::path& path);

struct GroupedPaths {
    std::vector<std::vector<std::filesystem::path>> groups;
    std::vector<std::filesystem::path> singles;
};

[[nodiscard]] GroupedPaths groupWrfPaths(const std::vector<std::filesystem::path>& paths);

class WrfFileSeries {
public:
    // paths must already be time-ordered (see groupWrfPaths) and contain 2+
    // entries. Only paths[0] is opened here, unless every file's valid time
    // can't be trusted to come from its filename alone (see the fast/eager
    // path split in the .cpp) - mirrors wrfseries.WRFFileSeries.__init__.
    explicit WrfFileSeries(std::vector<std::filesystem::path> paths);
    [[nodiscard]] const WrfFile& firstFile() const noexcept { return *files_.at(0); }
    [[nodiscard]] const std::vector<std::string>& times() const noexcept { return times_; }
    [[nodiscard]] const std::vector<WrfVariable>& variables() const noexcept { return variables_; }
    [[nodiscard]] std::string name() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return paths_.front(); }
    [[nodiscard]] std::vector<float> read(const std::string& variable, int timeIndex, int levelIndex = 0);
    [[nodiscard]] std::size_t openedFileCount() const noexcept { return files_.size(); }
    [[nodiscard]] bool isFileOpen(std::size_t index) const noexcept { return files_.count(index) != 0; }
private:
    WrfFile& fileAt(std::size_t index);
    std::vector<std::filesystem::path> paths_;
    std::unordered_map<std::size_t, std::unique_ptr<WrfFile>> files_;
    std::vector<std::string> times_;
    std::vector<WrfVariable> variables_;
    // (fileIndex, localTimeIndex) for each entry in times_/read()'s
    // timeIndex - identity in the fast per-file-is-one-timestep path,
    // stepped across files in the eager fallback path.
    std::vector<std::pair<std::size_t, int>> timeMap_;
};

}  // namespace wrftools
