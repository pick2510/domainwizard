#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wrftools {

// A variable's raw (undestaggered - see derived_variable.cpp's own note on
// why derived expressions must not go through reproject.cpp's usual
// vertical-destagger path) dimension: dimensionName is empty for a 2D
// variable, and levelCount is 1 in that case too.
struct VariableShape {
    std::string dimensionName;
    int levelCount{1};
};

struct Expr;  // AST node - defined only in derived_variable.cpp

// One `NAME = expr;` statement from a script, with any `NAME@attr = "...";`
// statements targeting it folded in.
struct DerivedVariableDef {
    std::string name;
    std::string units;
    std::string longName;
    std::shared_ptr<const Expr> expression;
    // true when `name` is also a source variable's own name (e.g.
    // `T2 = T2 - 273.15;`) - the expression's own operand reference to
    // `name` on the RHS still resolves to the ORIGINAL source variable
    // (see parseDerivedVariables), so this is a replacement, not a
    // self-referencing loop. The caller (reproject.cpp) uses this to skip
    // writing a separate pass-through copy of `name` even if it was also
    // checked/selected, and to fall back units/longName to the source
    // variable's own when this def doesn't set them itself.
    bool overridesSourceVariable{false};
};

// A small ncap2-like arithmetic-processor grammar for the Reproject tab's
// "Derived Variables" script box - see include/wrftools/reproject.hpp's
// ReprojectOptions::derivedVariablesScript and reproject_form.hpp. Grammar
// (standard C/ncap2-style precedence, lowest to highest):
//
//   script          := (statement ';')*
//   statement       := assignment | attrAssignment
//   assignment      := IDENT '=' expr
//   attrAssignment  := IDENT '@' IDENT '=' STRING
//
//   expr            := ternary
//   ternary         := logicalOr ('?' expr ':' expr)?      // right-assoc; elementwise select
//   logicalOr       := logicalAnd ('||' logicalAnd)*
//   logicalAnd      := equality ('&&' equality)*
//   equality        := comparison (('=='|'!=') comparison)*
//   comparison      := additive (('<'|'<='|'>'|'>=') additive)*
//   additive        := term (('+'|'-') term)*
//   term            := power (('*'|'/'|'%') power)*
//   power           := unary ('^' power)?                  // right-assoc
//   unary           := ('-'|'+'|'!')? primary
//   primary         := NUMBER | IDENT | IDENT '(' expr (',' expr)* ')' | '(' expr ')'
//   // '//' starts a line comment; NUMBER accepts an optional exponent (1e-3)
//
// Comparison/logical/'!' operators produce an elementwise 0.0/1.0 float
// array (so e.g. `T2 * (T2 > 273.15)` works as a mask, same idiom as
// ncap2/numpy); '?:' elementwise-selects between its two branches per pixel
// using the condition's 0.0/1.0 array.
//
// Builtin functions (fixed arity, applied elementwise): sqrt, exp, log
// (natural log), log10, abs, sin, cos, tan, asin, acos, atan, floor, ceil,
// round, sign (1 argument); pow(x, y), atan2(y, x), min(a, b), max(a, b)
// (2 arguments).
[[nodiscard]] std::vector<std::string> builtinFunctionNames();

// Parses `script`. `sourceShapes` gives every available source variable's
// raw dimension/level count by name (e.g. {"PH": {"bottom_top_stag", 65}},
// {"HGT": {"", 1}}). A later statement may reference an earlier one's name
// (chaining) - accepted names accumulate as parsing proceeds, seeded from
// sourceShapes.
//
// An assignment target may reuse an existing SOURCE variable's own name
// exactly once - e.g. `T2 = T2 - 273.15;` - matching ncap2's own "variables
// can be reassigned" behaviour; the resulting DerivedVariableDef has
// overridesSourceVariable set. The RHS's own reference to `T2` resolves to
// the ORIGINAL source variable, not the new definition (shapes_/known-names
// only gain the new entry once the whole RHS has been parsed), so this is a
// replacement, not infinite self-reference.
//
// Throws UserError (naming the offending line/column) on:
//  - a syntax error (unexpected token, unmatched paren, wrong argument
//    count to a builtin, ...);
//  - a reference to a name that is neither a source variable nor an
//    earlier statement's own name in this script, or an unknown function
//    name;
//  - an assignment target name already assigned earlier IN THIS SCRIPT -
//    whether that earlier assignment was itself a source-variable override
//    or a brand-new name; only ONE assignment per name is allowed, so a
//    source variable can be overridden but not reassigned twice;
//  - an `@attr` statement targeting a name not yet defined by an
//    assignment in this same script;
//  - an expression combining two operands (directly, or through a function
//    call's argument list) that name two DIFFERENT vertical dimensions -
//    mirrors ncap2's own non-conformable-arrays error. Any number of 2D/
//    scalar operands broadcast freely against at most one named dimension.
[[nodiscard]] std::vector<DerivedVariableDef> parseDerivedVariables(
    const std::string& script, const std::map<std::string, VariableShape>& sourceShapes);

// `def`'s own inferred output shape, computed while parsing.
[[nodiscard]] VariableShape shapeOf(const DerivedVariableDef& def);

// Evaluates one native-grid slice of `def` at `level` (indexed in def's own
// output dimension - see shapeOf). `resolve(name, level)` must return that
// identifier's native-grid slice; for an identifier whose own shape is 2D,
// `level` is ignored by evaluate() itself (broadcast is applied here, using
// each leaf's shape recorded at parse time - the resolver is never asked to
// know about broadcasting). Every slice `resolve` returns, and the result,
// have the same length (source grid width*height). Never throws for data
// reasons - a division or a builtin like log()/sqrt() outside its domain
// just produces the normal IEEE-754 inf/NaN, propagated like the rest of
// this app's float pipeline (see reproject.cpp's own NaN->fill-value
// handling) - but may propagate whatever `resolve` itself throws.
using OperandResolver = std::function<const std::vector<float>&(const std::string& name, int level)>;
[[nodiscard]] std::vector<float> evaluate(const DerivedVariableDef& def, int level, const OperandResolver& resolve);

}  // namespace wrftools
