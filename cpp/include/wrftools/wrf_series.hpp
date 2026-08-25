#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

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
    explicit WrfFileSeries(std::vector<std::filesystem::path> paths);
    [[nodiscard]] const WrfFile& firstFile() const noexcept { return *files_.at(0); }
    [[nodiscard]] const std::vector<std::string>& times() const noexcept { return times_; }
    [[nodiscard]] std::vector<float> read(const std::string& variable, int timeIndex, int levelIndex = 0);
    [[nodiscard]] std::size_t openedFileCount() const noexcept { return files_.size(); }
private:
    WrfFile& fileAt(std::size_t index);
    std::vector<std::filesystem::path> paths_;
    std::unordered_map<std::size_t, std::unique_ptr<WrfFile>> files_;
    std::vector<std::string> times_;
};

}  // namespace wrftools
