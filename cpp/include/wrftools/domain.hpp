#pragma once

#include "wrftools/crs.hpp"

#include <optional>
#include <string>
#include <vector>

namespace wrftools {

// Kept as an alias of Bounds2D (crs.hpp) rather than a separate type: a
// domain's bbox lives in the project's own projected CRS, same as any other
// projected bounds this app computes.
using Bounds = Bounds2D;

struct Domain {
    int id{};
    int parentId{};
    int ratio{1};
    int paddingLeft{};
    int paddingBottom{};
    // Computed by DomainProject::fillDomains() - display-only, not needed
    // for WPS placement (i_parent_start/j_parent_start come from
    // paddingLeft/paddingBottom alone). Mirrors project.py's
    // padding_right/padding_top.
    int paddingRight{};
    int paddingTop{};
    int columns{};
    int rows{};
    double dx{};
    double dy{};
    std::optional<Bounds> bounds;

    // Root-only projection fields (mirrors project.py: one projection per
    // project, defined on domain 1). mapProj is one of "lat-lon" | "lambert"
    // | "mercator" | "polar". Ignored on non-root domains.
    std::string mapProj{"lat-lon"};
    double trueLat1{};
    double trueLat2{};
    double standLon{};
    // Root: the user-set center. Non-root: computed by fillDomains() from
    // the domain's own bbox center.
    double centerLon{};
    double centerLat{};
};

// WPS-native root-first tree. Domain ids are one-based vector positions.
class DomainProject {
public:
    explicit DomainProject(std::vector<Domain> domains = {});
    [[nodiscard]] const std::vector<Domain>& domains() const noexcept { return domains_; }
    [[nodiscard]] std::vector<Domain>& domains() noexcept { return domains_; }
    void validate() const;
    void removeSubtree(int id);

    // The project's single CRS, defined by the root domain's projection
    // fields. Throws UserError if there are no domains. Mirrors
    // Project.projection.
    [[nodiscard]] Crs projection() const;

    // Computes bbox/cellSize/centerLon-Lat/paddingRight-Top/parentStart for
    // every domain, root-first (WPS guarantees parentId < id, so a single
    // forward pass is already in dependency order). Throws UserError for an
    // out-of-range parentId or a child that doesn't fit inside its parent's
    // bbox (tolerance: min(child dx, child dy) * 1e-6, relative to the
    // child - see project.py's fill_domains). Mirrors Project.fill_domains.
    void fillDomains();

private:
    std::vector<Domain> domains_;
};

}  // namespace wrftools
