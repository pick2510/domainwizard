#include "wrftools/wps_namelist.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace wrftools {
namespace {
using Group = std::map<std::string, std::string>;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r,");
    return value.substr(first, last - first + 1);
}

std::map<std::string, Group> parseGroups(std::istream& input) {
    std::map<std::string, Group> groups;
    std::string line, current;
    while (std::getline(input, line)) {
        const auto comment = line.find('!');
        if (comment != std::string::npos) line.resize(comment);
        const auto clean = trim(line);
        if (clean.empty()) continue;
        if (clean.front() == '&') { current = clean.substr(1); continue; }
        if (clean == "/") { current.clear(); continue; }
        if (current.empty()) continue;
        const auto equals = clean.find('=');
        if (equals == std::string::npos) continue;
        auto key = trim(clean.substr(0, equals));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        groups[current][key] = trim(clean.substr(equals + 1));
    }
    return groups;
}

std::vector<std::string> values(const Group& group, const std::string& key) {
    const auto found = group.find(key);
    if (found == group.end()) throw UserError("Invalid namelist, variable " + key + " not found.");
    std::vector<std::string> result;
    std::stringstream input(found->second);
    std::string item;
    while (std::getline(input, item, ',')) {
        item = trim(item);
        if (item.size() >= 2 && (item.front() == '\'' || item.front() == '"') && item.back() == item.front()) item = item.substr(1, item.size() - 2);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

int integer(const Group& group, const std::string& key) { return std::stoi(values(group, key).front()); }
double real(const Group& group, const std::string& key) { return std::stod(values(group, key).front()); }
template <typename T> std::string joined(const std::vector<T>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << ", "; out << values[i]; }
    return out.str();
}
}

WpsProject readWpsNamelist(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw UserError("Namelist file does not exist: " + path.string());
    const auto groups = parseGroups(input);
    const auto share = groups.find("share"), geogrid = groups.find("geogrid");
    if (share == groups.end() || geogrid == groups.end()) throw UserError("Invalid namelist, share or geogrid section not found.");
    const int maxDomains = integer(share->second, "max_dom");
    const auto parent = values(geogrid->second, "parent_id");
    const auto ratio = values(geogrid->second, "parent_grid_ratio");
    const auto startI = values(geogrid->second, "i_parent_start");
    const auto startJ = values(geogrid->second, "j_parent_start");
    const auto eWe = values(geogrid->second, "e_we");
    const auto eSn = values(geogrid->second, "e_sn");
    if (static_cast<int>(parent.size()) != maxDomains || ratio.size() != parent.size() || startI.size() != parent.size() || startJ.size() != parent.size() || eWe.size() != parent.size() || eSn.size() != parent.size())
        throw UserError("max_dom does not match the number of geogrid domain values.");
    std::vector<Domain> domains;
    for (int i = 0; i < maxDomains; ++i) {
        Domain domain{.id = i + 1, .parentId = std::stoi(parent[i]), .ratio = std::stoi(ratio[i]), .paddingLeft = std::stoi(startI[i]) - 1, .paddingBottom = std::stoi(startJ[i]) - 1, .columns = std::stoi(eWe[i]) - 1, .rows = std::stoi(eSn[i]) - 1, .bounds = std::nullopt};
        if (i == 0) { domain.dx = real(geogrid->second, "dx"); domain.dy = real(geogrid->second, "dy"); }
        domains.push_back(domain);
    }
    const auto optionalReal = [&geogrid](const std::string& name) { const auto it = geogrid->second.find(name); return it == geogrid->second.end() ? 0.0 : std::stod(values(geogrid->second, name).front()); };
    return {.domains = DomainProject(std::move(domains)), .mapProjection = values(geogrid->second, "map_proj").front(), .referenceLongitude = real(geogrid->second, "ref_lon"), .referenceLatitude = real(geogrid->second, "ref_lat"), .trueLatitude1 = optionalReal("truelat1"), .trueLatitude2 = optionalReal("truelat2"), .standardLongitude = optionalReal("stand_lon")};
}

void writeWpsNamelist(const WpsProject& project, const std::filesystem::path& path) {
    project.domains.validate();
    const auto& domains = project.domains.domains();
    if (domains.empty()) throw UserError("Cannot export an empty domain project.");
    std::vector<int> parent, ratio, startI, startJ, eWe, eSn;
    for (const auto& domain : domains) { parent.push_back(domain.parentId); ratio.push_back(domain.ratio); startI.push_back(domain.paddingLeft + 1); startJ.push_back(domain.paddingBottom + 1); eWe.push_back(domain.columns + 1); eSn.push_back(domain.rows + 1); }
    std::ofstream out(path);
    if (!out) throw UserError("Could not write namelist: " + path.string());
    out << "&share\n max_dom = " << domains.size() << ",\n/\n\n&geogrid\n"
        << " parent_id = " << joined(parent) << ",\n parent_grid_ratio = " << joined(ratio) << ",\n i_parent_start = " << joined(startI) << ",\n j_parent_start = " << joined(startJ) << ",\n e_we = " << joined(eWe) << ",\n e_sn = " << joined(eSn) << ",\n"
        << " map_proj = '" << project.mapProjection << "',\n dx = " << domains.front().dx << ",\n dy = " << domains.front().dy << ",\n ref_lon = " << project.referenceLongitude << ",\n ref_lat = " << project.referenceLatitude << ",\n"
        << " truelat1 = " << project.trueLatitude1 << ",\n truelat2 = " << project.trueLatitude2 << ",\n stand_lon = " << project.standardLongitude << ",\n/\n";
}
}  // namespace wrftools
