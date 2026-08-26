#include "wrftools/wrf_source.hpp"
#include "wrftools/error.hpp"

namespace wrftools {
namespace {
class SingleFileSource final : public WrfSource {
public:
    explicit SingleFileSource(std::filesystem::path path) : file_(std::move(path)) {}
    const std::vector<WrfVariable>& variables() const override { return file_.variables(); }
    const std::array<double, 6>& geotransform() const override { return file_.geotransform(); }
    std::array<int, 2> size() const override { return file_.size(); }
    const std::string& projectionWkt() const override { return file_.projectionWkt(); }
    const GeographicBounds& geographicBounds() const override { return file_.geographicBounds(); }
    const std::vector<std::string>* seriesTimes() const override { return nullptr; }
    std::string displayName() const override { return file_.path().filename().string(); }
    std::vector<float> read(const std::string& variable, int timeIndex, int levelIndex) override { return file_.read(variable, timeIndex, levelIndex); }

private:
    WrfFile file_;
};

class SeriesSource final : public WrfSource {
public:
    explicit SeriesSource(std::vector<std::filesystem::path> paths) : series_(std::move(paths)) {}
    const std::vector<WrfVariable>& variables() const override { return series_.variables(); }
    const std::array<double, 6>& geotransform() const override { return series_.firstFile().geotransform(); }
    std::array<int, 2> size() const override { return series_.firstFile().size(); }
    const std::string& projectionWkt() const override { return series_.firstFile().projectionWkt(); }
    const GeographicBounds& geographicBounds() const override { return series_.firstFile().geographicBounds(); }
    const std::vector<std::string>* seriesTimes() const override { return &series_.times(); }
    std::string displayName() const override { return series_.name(); }
    std::vector<float> read(const std::string& variable, int timeIndex, int levelIndex) override { return series_.read(variable, timeIndex, levelIndex); }

private:
    WrfFileSeries series_;
};
}  // namespace

WrfSource& WrfSourceRegistry::open(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) throw UserError("No files given to open.");
    const auto key = paths.front().string();
    if (const auto found = sources_.find(key); found != sources_.end()) return *found->second;
    std::unique_ptr<WrfSource> source = paths.size() == 1
        ? std::unique_ptr<WrfSource>(std::make_unique<SingleFileSource>(paths.front()))
        : std::unique_ptr<WrfSource>(std::make_unique<SeriesSource>(paths));
    auto [it, inserted] = sources_.emplace(key, std::move(source));
    return *it->second;
}

std::vector<std::string> WrfSourceRegistry::openPaths() const {
    std::vector<std::string> result;
    result.reserve(sources_.size());
    for (const auto& [path, unused] : sources_) { static_cast<void>(unused); result.push_back(path); }
    return result;
}

void WrfSourceRegistry::invalidate(const std::string& path) { sources_.erase(path); }
void WrfSourceRegistry::clear() { sources_.clear(); }

}  // namespace wrftools
