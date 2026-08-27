#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wrftools {

// Minimal RAII wrapper over the netCDF-C API, scoped to exactly what the
// w2w LCZ port needs (see PORT_W2W.MD Stage 0) - not a general-purpose
// NetCDF library. WRF geo_em files are always classic-model-shaped NetCDF4
// with at most 4-D variables (Time, {land_cat|month|num_urb_params},
// south_north, west_east) and only NC_CHAR/NC_INT/NC_FLOAT/NC_DOUBLE
// attributes/variables in practice, so this never needs groups, compound
// types, or string-array attributes.
class NetcdfFile {
public:
    enum class Mode { ReadOnly, ReadWrite };

    // NetCDF's own external type codes (NC_CHAR, NC_INT, NC_FLOAT, ...),
    // reused directly rather than wrapped in a new enum - callers that
    // care compare against the NC_* constants from <netcdf.h>.
    using NcType = int;

    struct Dimension {
        std::string name;
        std::size_t length{};
        bool isUnlimited{};
    };

    struct Variable {
        std::string name;
        NcType type{};
        std::vector<std::string> dimensionNames;  // outermost first, matching on-disk order
    };

    // Attribute values are always small (WRF's largest is a 16-element
    // corner_lats/corner_lons array) - one struct covers text and
    // numeric-array attributes without a variant. `type` is the on-disk
    // NC_* type; `text` is populated for NC_CHAR, `numbers` for anything
    // else (a scalar attribute is a one-element vector).
    struct Attribute {
        std::string name;
        NcType type{};
        std::string text;
        std::vector<double> numbers;
    };

    static NetcdfFile open(const std::filesystem::path& path, Mode mode);

    ~NetcdfFile();
    NetcdfFile(NetcdfFile&&) noexcept;
    NetcdfFile& operator=(NetcdfFile&&) noexcept;
    NetcdfFile(const NetcdfFile&) = delete;
    NetcdfFile& operator=(const NetcdfFile&) = delete;

    // ---- Introspection ----
    [[nodiscard]] std::vector<Dimension> dimensions() const;
    [[nodiscard]] std::vector<Variable> variables() const;
    [[nodiscard]] bool hasVariable(const std::string& name) const;
    [[nodiscard]] Variable variable(const std::string& name) const;  // throws UserError if absent
    [[nodiscard]] std::vector<std::size_t> shape(const std::string& variableName) const;

    // ---- Attributes. Pass an empty variableName for a global attribute
    // (NC_GLOBAL), matching xarray's/w2w's own "dst_data.attrs[...]"
    // vs. "dst_data[var].attrs[...]" split. ----
    [[nodiscard]] bool hasAttribute(const std::string& variableName, const std::string& attrName) const;
    [[nodiscard]] Attribute getAttribute(const std::string& variableName, const std::string& attrName) const;
    void putAttribute(const std::string& variableName, const Attribute& attribute);

    // ---- Data. netCDF-C's typed get/put converts from/to the variable's
    // actual on-disk type, so readFloat works even against an int/double
    // variable (WRF stores nearly everything as float, but this isn't
    // relied on). ----
    [[nodiscard]] std::vector<float> readFloat(const std::string& variableName) const;
    [[nodiscard]] std::vector<double> readDouble(const std::string& variableName) const;
    [[nodiscard]] std::vector<std::int32_t> readInt(const std::string& variableName) const;

    // Writes must match the variable's already-defined shape exactly - use
    // resizeDimension (below) to change a variable's shape.
    void writeFloat(const std::string& variableName, const std::vector<float>& data);
    void writeDouble(const std::string& variableName, const std::vector<double>& data);
    void writeInt(const std::string& variableName, const std::vector<std::int32_t>& data);

    // ---- Structural additions to an already-open, already-existing file.
    // Unlike resizeDimension (which changes an EXISTING dimension's size,
    // something netCDF classic genuinely cannot do without rebuilding the
    // file), netCDF-C supports adding a brand-new dimension/variable to an
    // existing file in place via nc_redef/nc_enddef - no rebuild needed.
    // Each call pays its own redef/enddef round-trip; fine for the small,
    // fixed number of new variables the LCZ pipeline adds (FRC_URB2D,
    // URB_PARAM) rather than a hot path. ----
    void defineDimension(const std::string& name, std::size_t length);
    void defineVariable(const std::string& name, NcType type, const std::vector<std::string>& dimensionNames);

    void close();  // idempotent; also called by the destructor

    // ---- Whole-file operations ----

    // Byte-for-byte file copy (overwrites `dst` if present) - the cheapest
    // way to get xarray's ".copy()"-then-mutate-in-place semantics for the
    // common case where only a handful of variables/attributes change and
    // everything else should survive untouched.
    static void copyFile(const std::filesystem::path& src, const std::filesystem::path& dst);

    // NetCDF dimensions can't be resized in place - this rebuilds `path`
    // (in place, via a temporary file swapped in on success) with
    // `dimensionName` resized to `newLength`. Every dimension, variable,
    // and attribute is copied unchanged EXCEPT variables that use the
    // resized dimension: for each of those, `newData` is called with the
    // variable's name and must return the variable's full new data (sized
    // for `newLength`, float - matching every real case this is used for:
    // LANDUSEF's land_cat growth). A variable using the resized dimension
    // that `newData` returns std::nullopt for is defined at its new shape
    // but left unwritten (zero-filled by netCDF) - not used by any current
    // caller, but kept rather than throwing, since a future caller may
    // legitimately not care about a given such variable's contents.
    static void resizeDimension(const std::filesystem::path& path, const std::string& dimensionName, std::size_t newLength,
        const std::function<std::optional<std::vector<float>>(const std::string& variableName)>& newData);

    // A variable to be (re)defined from scratch during rebuildStructure,
    // rather than copied unchanged from the source file.
    struct VariableOverride {
        std::string name;
        NcType type{};
        std::vector<std::string> dimensionNames;  // may include a brand-new dimension being added
        std::vector<float> data;
    };

    // Generalizes resizeDimension for the case where a real-world file
    // already has its OWN differently-shaped definition of a variable
    // this port needs to (re)create - e.g. some geo_em files already
    // carry FRC_URB2D/URB_PARAM from geogrid's own default (non-LCZ)
    // urban parameterization, with URB_PARAM's parameter-count dimension
    // sized for that scheme, not this one's 132. xarray's own Dataset
    // model handles this transparently (assigning a `(new_dims,
    // new_data)` tuple to an existing variable name just redefines it);
    // netCDF-C's nc_def_var errors outright if the name is already
    // defined, so this rebuilds the file instead - same "copy everything
    // unchanged except..." shape as resizeDimension, except here the
    // exceptions are `variableOverrides` (defined fresh with their own
    // possibly-new dimensions, instead of copied from source) on top of
    // one optionally-resized existing dimension. A dimension left with no
    // remaining variable referencing it afterward (e.g. a legacy
    // FRC_URB2D/URB_PARAM scheme's own now-unused dimension) is copied
    // over unchanged anyway rather than pruned - harmless (an unused
    // dimension is valid NetCDF) and simpler than detecting orphans, at
    // the cost of a cosmetic difference from xarray's own output (which
    // does prune it).
    // `newDimensions` are brand-new dimensions (name, length) the
    // overrides need (e.g. {"num_urb_params", 132}) - defined once, after
    // the normal (possibly-resized) dimension set.
    static void rebuildStructure(const std::filesystem::path& path, const std::string& resizedDimensionName, std::size_t resizedDimensionNewLength,
        const std::function<std::optional<std::vector<float>>(const std::string& variableName)>& resizedDimensionNewData,
        const std::vector<std::pair<std::string, std::size_t>>& newDimensions, const std::vector<VariableOverride>& variableOverrides);

private:
    NetcdfFile(int ncid, std::filesystem::path path, Mode mode);

    int ncid_{-1};
    std::filesystem::path path_;
    Mode mode_{Mode::ReadOnly};
};

}  // namespace wrftools
