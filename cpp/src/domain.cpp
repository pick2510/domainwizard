#include "wrftools/domain.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace wrftools {

bool Bounds::contains(const Bounds& other, double tolerance) const {
    return other.minX >= minX - tolerance && other.minY >= minY - tolerance &&
           other.maxX <= maxX + tolerance && other.maxY <= maxY + tolerance;
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
        if (parent.bounds && domain.bounds && !parent.bounds->contains(*domain.bounds))
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

}  // namespace wrftools
