#include "wrftools/wps_namelist.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
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

// Whitespace-only trim, deliberately NOT stripping a trailing comma like
// trim() does: used while joining a value that may still be wrapped across
// further lines, where a trailing comma is the only thing separating this
// line's last element from the next line's first one. Stripping it early
// (as trim() would) silently fuses two array elements into one token, which
// values() then can't split back apart.
std::string trimWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Matches a Fortran namelist assignment starting a new variable, e.g.
// "parent_id = 1, 1," or "dx=6250.0" - used to tell a continuation line
// (part of the previous variable's value, wrapped across lines - valid and
// not uncommon Fortran namelist syntax) from the start of the next one.
const std::regex kAssignmentStart(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$)");

std::map<std::string, Group> parseGroups(std::istream& input) {
    std::map<std::string, Group> groups;
    std::string line, current, pendingKey, pendingValue;
    const auto flushPending = [&] { if (!pendingKey.empty()) { groups[current][pendingKey] = trim(pendingValue); pendingKey.clear(); pendingValue.clear(); } };
    while (std::getline(input, line)) {
        const auto comment = line.find('!');
        if (comment != std::string::npos) line.resize(comment);
        const auto clean = trimWhitespace(line);
        if (clean.empty()) continue;
        if (clean.front() == '&') { flushPending(); current = clean.substr(1); continue; }
        if (clean == "/") { flushPending(); current.clear(); continue; }
        if (current.empty()) continue;
        std::smatch match;
        if (std::regex_match(clean, match, kAssignmentStart)) {
            flushPending();
            pendingKey = lowered(match[1].str());
            pendingValue = match[2].str();
        } else if (!pendingKey.empty()) {
            // A continuation of the current variable's value, wrapped onto
            // its own line - e.g. WPS namelists commonly wrap a long
            // i_parent_start/j_parent_start array this way.
            pendingValue += ' ';
            pendingValue += clean;
        }
    }
    flushPending();
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
    const auto optionalReal = [&geogrid](const std::string& name) { const auto it = geogrid->second.find(name); return it == geogrid->second.end() ? 0.0 : std::stod(values(geogrid->second, name).front()); };
    std::vector<Domain> domains;
    for (int i = 0; i < maxDomains; ++i) {
        Domain domain{.id = i + 1, .parentId = std::stoi(parent[i]), .ratio = std::stoi(ratio[i]), .paddingLeft = std::stoi(startI[i]) - 1, .paddingBottom = std::stoi(startJ[i]) - 1, .columns = std::stoi(eWe[i]) - 1, .rows = std::stoi(eSn[i]) - 1, .bounds = std::nullopt};
        if (i == 0) {
            domain.dx = real(geogrid->second, "dx"); domain.dy = real(geogrid->second, "dy");
            domain.mapProj = values(geogrid->second, "map_proj").front();
            domain.trueLat1 = optionalReal("truelat1"); domain.trueLat2 = optionalReal("truelat2"); domain.standLon = optionalReal("stand_lon");
            domain.centerLon = real(geogrid->second, "ref_lon"); domain.centerLat = real(geogrid->second, "ref_lat");
        }
        domains.push_back(domain);
    }
    return {.domains = DomainProject(std::move(domains))};
}

void writeWpsNamelist(const WpsProject& project, const std::filesystem::path& path) {
    project.domains.validate();
    const auto& domains = project.domains.domains();
    if (domains.empty()) throw UserError("Cannot export an empty domain project.");
    std::vector<int> parent, ratio, startI, startJ, eWe, eSn;
    for (const auto& domain : domains) { parent.push_back(domain.parentId); ratio.push_back(domain.ratio); startI.push_back(domain.paddingLeft + 1); startJ.push_back(domain.paddingBottom + 1); eWe.push_back(domain.columns + 1); eSn.push_back(domain.rows + 1); }
    const auto& root = domains.front();
    std::ofstream out(path);
    if (!out) throw UserError("Could not write namelist: " + path.string());
    // Shape (group set, nocolons, the synthetic &metgrid group) matches
    // gis4wrf.core.transforms.project_to_wps_namelist's own export exactly:
    // that function reconstructs a namelist from Project fields too, not a
    // preserve-and-patch of whatever was imported, so this isn't a
    // C++-only gap - fields like start_date/geog_data_path/&ungrib that
    // aren't derived from Domain aren't part of either reference's export.
    out << "&share\n nocolons = .true.,\n max_dom = " << domains.size() << ",\n/\n\n&geogrid\n"
        << " parent_id = " << joined(parent) << ",\n parent_grid_ratio = " << joined(ratio) << ",\n i_parent_start = " << joined(startI) << ",\n j_parent_start = " << joined(startJ) << ",\n e_we = " << joined(eWe) << ",\n e_sn = " << joined(eSn) << ",\n"
        << " map_proj = '" << root.mapProj << "',\n dx = " << root.dx << ",\n dy = " << root.dy << ",\n ref_lon = " << root.centerLon << ",\n ref_lat = " << root.centerLat << ",\n"
        << " truelat1 = " << root.trueLat1 << ",\n truelat2 = " << root.trueLat2 << ",\n stand_lon = " << root.standLon << ",\n/\n\n&metgrid\n fg_name = 'FILE',\n/\n";
}
}  // namespace wrftools
