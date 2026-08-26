#include "wrftools/domain.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace wrftools {
namespace {
bool boundsContains(const Bounds& outer, const Bounds& inner, double tolerance) {
    return inner.minX >= outer.minX - tolerance && inner.minY >= outer.minY - tolerance &&
           inner.maxX <= outer.maxX + tolerance && inner.maxY <= outer.maxY + tolerance;
}
}

DomainProject::DomainProject(std::vector<Domain> domains) : domains_(std::move(domains)) {
    validate();
}

void DomainProject::validate() const {
    for (std::size_t index = 0; index < domains_.size(); ++index) {
        const auto& domain = domains_[index];
        const int expectedId = static_cast<int>(index) + 1;
        if (domain.id != expectedId) throw UserError("Domain ids must be root-first consecutive WPS ids.");
        if (domain.columns <= 0 || domain.rows <= 0) throw UserError("Domain size must be positive.");
        if (index == 0) {
            if (domain.parentId != 1) throw UserError("The root domain's parent_id must be 1.");
            continue;
        }
        if (domain.parentId < 1 || domain.parentId >= domain.id)
            throw UserError("Every child must reference an earlier domain as its parent.");
        if (domain.ratio < 1) throw UserError("A nesting ratio must be at least 1.");
        const auto& parent = domains_[static_cast<std::size_t>(domain.parentId - 1)];
        if (parent.bounds && domain.bounds && !boundsContains(*parent.bounds, *domain.bounds, 1e-9))
            throw UserError("A child domain must be contained by its parent.");
    }
}

void DomainProject::removeSubtree(int id) {
    if (id < 1 || id > static_cast<int>(domains_.size())) throw UserError("Selected domain does not exist.");
    std::unordered_set<int> removed{id};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& domain : domains_)
            if (!removed.contains(domain.id) && removed.contains(domain.parentId)) {
                removed.insert(domain.id);
                changed = true;
            }
    }
    std::vector<Domain> survivors;
    survivors.reserve(domains_.size() - removed.size());
    for (const auto& domain : domains_) if (!removed.contains(domain.id)) survivors.push_back(domain);
    std::unordered_map<int, int> renumbered;
    for (std::size_t i = 0; i < survivors.size(); ++i) renumbered.emplace(survivors[i].id, static_cast<int>(i) + 1);
    for (auto& domain : survivors) {
        domain.id = renumbered.at(domain.id);
        domain.parentId = renumbered.at(domain.parentId);
    }
    domains_ = std::move(survivors);
    validate();
}

Crs DomainProject::projection() const {
    if (domains_.empty()) throw UserError("Domains are not configured yet");
    const auto& root = domains_.front();
    if (root.mapProj == "lambert") return Crs::lambert(root.trueLat1, root.trueLat2, {root.standLon, root.centerLat});
    if (root.mapProj == "mercator") return Crs::mercator(root.trueLat1, root.centerLon);
    if (root.mapProj == "polar") return Crs::polar(root.trueLat1, root.standLon);
    if (root.mapProj == "lat-lon") return Crs::lonLat();
    throw UserError("Invalid projection: " + root.mapProj);
}

void DomainProject::fillDomains() {
    if (domains_.empty()) throw UserError("Domains are not configured yet");
    auto& root = domains_.front();
    if (root.parentId != 1) throw UserError("The first domain in the list must be the root domain (parent_id 1)");

    const auto projection = this->projection();
    const auto center = projection.toXy({root.centerLon, root.centerLat});
    root.bounds = Bounds{
        center.x - root.dx * root.columns / 2.0, center.y - root.dy * root.rows / 2.0,
        center.x + root.dx * root.columns / 2.0, center.y + root.dy * root.rows / 2.0};

    for (std::size_t index = 1; index < domains_.size(); ++index) {
        auto& domain = domains_[index];
        const int domainNumber = static_cast<int>(index) + 1;
        if (domain.parentId < 1 || domain.parentId >= domainNumber)
            throw UserError("Domain " + std::to_string(domainNumber) + " has an invalid parent_id (" + std::to_string(domain.parentId) +
                             "): it must refer to an earlier, already-defined domain");
        auto& parent = domains_[static_cast<std::size_t>(domain.parentId - 1)];
        if (!parent.bounds) throw UserError("Domain " + std::to_string(domainNumber) + "'s parent has no computed bounds.");

        domain.dx = parent.dx / domain.ratio;
        domain.dy = parent.dy / domain.ratio;
        const double minX = parent.bounds->minX + domain.paddingLeft * parent.dx;
        const double minY = parent.bounds->minY + domain.paddingBottom * parent.dy;
        domain.bounds = Bounds{minX, minY, minX + domain.columns * domain.dx, minY + domain.rows * domain.dy};

        // A child domain must lie entirely within its parent's grid -
        // required by WPS/geogrid.exe, but nothing upstream of this point
        // enforces it. Tolerance is relative to the CHILD's own cell size
        // (matching project.py's fill_domains), to absorb floating-point
        // rounding without masking a real out-of-bounds placement.
        const double tolerance = std::min(domain.dx, domain.dy) * 1e-6;
        if (!boundsContains(*parent.bounds, *domain.bounds, tolerance))
            throw UserError("Domain " + std::to_string(domainNumber) + " does not fit within its parent domain " +
                             std::to_string(domain.parentId) + ": check its nesting ratio, position within parent, and size.");

        const auto center2 = projection.toLonLat({(domain.bounds->minX + domain.bounds->maxX) / 2.0, (domain.bounds->minY + domain.bounds->maxY) / 2.0});
        domain.centerLon = center2.lon;
        domain.centerLat = center2.lat;

        // Display-only (WPS placement uses paddingLeft/paddingBottom alone
        // via parentStart below), computed here so it's populated
        // regardless of whether this project came from a namelist import or
        // interactive editing.
        domain.paddingRight = static_cast<int>(std::lround((parent.bounds->maxX - domain.bounds->maxX) / parent.dx));
        domain.paddingTop = static_cast<int>(std::lround((parent.bounds->maxY - domain.bounds->maxY) / parent.dy));
    }
}

}  // namespace wrftools
