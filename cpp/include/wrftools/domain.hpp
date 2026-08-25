#pragma once

#include <optional>
#include <string>
#include <vector>

namespace wrftools {

struct Bounds {
    double minX{};
    double minY{};
    double maxX{};
    double maxY{};
    [[nodiscard]] bool contains(const Bounds& other, double tolerance = 1e-9) const;
};

struct Domain {
    int id{};
    int parentId{};
    int ratio{1};
    int paddingLeft{};
    int paddingBottom{};
    int columns{};
    int rows{};
    double dx{};
    double dy{};
    std::optional<Bounds> bounds;
};

// WPS-native root-first tree. Domain ids are one-based vector positions.
class DomainProject {
public:
    explicit DomainProject(std::vector<Domain> domains = {});
    [[nodiscard]] const std::vector<Domain>& domains() const noexcept { return domains_; }
    [[nodiscard]] std::vector<Domain>& domains() noexcept { return domains_; }
    void validate() const;
    void removeSubtree(int id);

private:
    std::vector<Domain> domains_;
};

}  // namespace wrftools
