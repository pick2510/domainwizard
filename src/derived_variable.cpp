#include "wrftools/derived_variable.hpp"
#include "wrftools/error.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wrftools {

enum class UnaryOp { Negate, Plus, Not };
enum class BinaryOp { Add, Subtract, Multiply, Divide, Modulo, Power, Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, And, Or };
enum class NodeKind { Number, Identifier, Unary, Binary, Ternary, Call };

// The AST node type declared (incomplete) in derived_variable.hpp - defined
// here since only this file needs to know its shape. One node kind per
// grammar production (see the hpp's grammar comment); `shape` is the node's
// own inferred output shape, computed bottom-up while parsing so shapeOf()
// is just `def.expression->shape` and evaluate() can look a leaf's
// broadcast rule up without re-deriving it.
struct Expr {
    NodeKind kind{};
    double number{};
    std::string identifier;
    UnaryOp unaryOp{};
    BinaryOp binaryOp{};
    std::shared_ptr<Expr> a, b, c;  // unary: a. binary: a op b. ternary: a ? b : c.
    std::string functionName;
    std::vector<std::shared_ptr<Expr>> args;
    VariableShape shape;
};

namespace {

// Fixed arity per builtin - also doubles as the set of recognized function
// names (an unrecognized call target is a parse-time UserError, same as an
// unrecognized identifier).
const std::map<std::string, int>& functionArity() {
    static const std::map<std::string, int> table{
        {"sqrt", 1}, {"exp", 1}, {"log", 1}, {"log10", 1}, {"abs", 1}, {"sin", 1}, {"cos", 1}, {"tan", 1}, {"asin", 1}, {"acos", 1}, {"atan", 1},
        {"floor", 1}, {"ceil", 1}, {"round", 1}, {"sign", 1}, {"pow", 2}, {"atan2", 2}, {"min", 2}, {"max", 2}};
    return table;
}

// ---- tokenizer --------------------------------------------------------

enum class TokKind { Number, String, Ident, Punct, End };

struct Token {
    TokKind kind{TokKind::End};
    std::string text;
    double number{};
    int line{1};
    int column{1};
};

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    int line = 1, column = 1;
    auto step = [&] {
        if (i < src.size()) { if (src[i] == '\n') { ++line; column = 1; } else { ++column; } ++i; }
    };
    while (i < src.size()) {
        const char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) { step(); continue; }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') step();
            continue;
        }
        const int startLine = line, startColumn = column;
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            const std::size_t start = i;
            while (i < src.size() && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) step();
            if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
                step();
                if (i < src.size() && (src[i] == '+' || src[i] == '-')) step();
                while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) step();
            }
            const auto text = src.substr(start, i - start);
            Token token{TokKind::Number, text, std::stod(text), startLine, startColumn};
            tokens.push_back(token);
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = i;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) step();
            tokens.push_back({TokKind::Ident, src.substr(start, i - start), 0.0, startLine, startColumn});
            continue;
        }
        if (c == '"') {
            step();
            const std::size_t start = i;
            while (i < src.size() && src[i] != '"') step();
            if (i >= src.size())
                throw UserError("Derived variable script error at line " + std::to_string(startLine) + ", column " + std::to_string(startColumn) +
                                 ": unterminated string literal.");
            const auto text = src.substr(start, i - start);
            step();  // closing quote
            tokens.push_back({TokKind::String, text, 0.0, startLine, startColumn});
            continue;
        }
        static const std::vector<std::string> kTwoCharOps{"==", "!=", "<=", ">=", "&&", "||"};
        bool matchedTwoChar = false;
        if (i + 1 < src.size()) {
            const auto two = src.substr(i, 2);
            if (std::find(kTwoCharOps.begin(), kTwoCharOps.end(), two) != kTwoCharOps.end()) {
                tokens.push_back({TokKind::Punct, two, 0.0, startLine, startColumn});
                step();
                step();
                matchedTwoChar = true;
            }
        }
        if (matchedTwoChar) continue;
        static const std::string kOneCharOps = "=@;,()+-*/%^?:<>!";
        if (kOneCharOps.find(c) != std::string::npos) {
            tokens.push_back({TokKind::Punct, std::string(1, c), 0.0, startLine, startColumn});
            step();
            continue;
        }
        throw UserError("Derived variable script error at line " + std::to_string(startLine) + ", column " + std::to_string(startColumn) +
                         ": unexpected character '" + std::string(1, c) + "'.");
    }
    tokens.push_back({TokKind::End, "", 0.0, line, column});
    return tokens;
}

// ---- parser -------------------------------------------------------------

class Parser {
public:
    Parser(const std::string& script, std::map<std::string, VariableShape> shapes) : shapes_(std::move(shapes)), tokens_(tokenize(script)) {}

    std::vector<DerivedVariableDef> parseScript() {
        std::vector<DerivedVariableDef> defs;
        std::map<std::string, std::size_t> indexByName;
        while (!atEnd()) {
            const auto nameToken = expectIdentToken("a variable name");
            if (isPunct("@")) {
                advance();
                const auto attrToken = expectIdentToken("an attribute name after '@'");
                expectPunct("=");
                const auto valueToken = current();
                if (valueToken.kind != TokKind::String) error(valueToken, "expected a quoted string value");
                advance();
                const auto found = indexByName.find(nameToken.text);
                if (found == indexByName.end())
                    error(nameToken, "'@" + attrToken.text + "' targets '" + nameToken.text + "', which has no assignment earlier in this script");
                auto& def = defs[found->second];
                if (attrToken.text == "units") def.units = valueToken.text;
                else if (attrToken.text == "long_name") def.longName = valueToken.text;
                else error(attrToken, "unsupported attribute '" + attrToken.text + "' (only 'units' and 'long_name' are supported)");
            } else {
                expectPunct("=");
                if (shapes_.count(nameToken.text))
                    error(nameToken, "'" + nameToken.text + "' is already defined (a source variable, or an earlier statement in this script)");
                auto expression = parseExpr();
                DerivedVariableDef def;
                def.name = nameToken.text;
                def.expression = expression;
                shapes_[nameToken.text] = expression->shape;
                indexByName[nameToken.text] = defs.size();
                defs.push_back(std::move(def));
            }
            expectPunct(";");
        }
        return defs;
    }

private:
    std::map<std::string, VariableShape> shapes_;
    std::vector<Token> tokens_;
    std::size_t pos_{};

    [[nodiscard]] const Token& current() const { return tokens_[pos_]; }
    [[nodiscard]] bool atEnd() const { return current().kind == TokKind::End; }
    void advance() {
        if (pos_ + 1 < tokens_.size()) ++pos_;
    }
    [[nodiscard]] bool isPunct(const std::string& text) const { return current().kind == TokKind::Punct && current().text == text; }

    [[noreturn]] void error(const Token& at, const std::string& message) const {
        throw UserError("Derived variable script error at line " + std::to_string(at.line) + ", column " + std::to_string(at.column) + ": " + message);
    }

    Token expectIdentToken(const std::string& what) {
        if (current().kind != TokKind::Ident) error(current(), "expected " + what);
        const auto token = current();
        advance();
        return token;
    }
    void expectPunct(const std::string& text) {
        if (!isPunct(text)) error(current(), "expected '" + text + "'");
        advance();
    }

    // Any number of 2D/scalar operands (empty dimensionName) broadcast
    // freely against at most one distinct named vertical dimension - two
    // operands naming DIFFERENT dimensions can never be combined
    // elementwise (mirrors ncap2's own non-conformable-arrays error).
    VariableShape combineShapes(const VariableShape& left, const VariableShape& right, const Token& at) const {
        if (left.dimensionName.empty()) return right;
        if (right.dimensionName.empty()) return left;
        if (left.dimensionName == right.dimensionName) {
            if (left.levelCount != right.levelCount)
                error(at, "vertical dimension '" + left.dimensionName + "' has inconsistent lengths (" + std::to_string(left.levelCount) + " vs " +
                              std::to_string(right.levelCount) + ") between operands");
            return left;
        }
        error(at, "cannot combine operands on different vertical dimensions '" + left.dimensionName + "' and '" + right.dimensionName + "'");
    }

    std::shared_ptr<Expr> parseExpr() { return parseTernary(); }

    std::shared_ptr<Expr> parseTernary() {
        auto condition = parseLogicalOr();
        if (isPunct("?")) {
            const auto token = current();
            advance();
            auto thenExpr = parseExpr();
            expectPunct(":");
            auto elseExpr = parseExpr();
            auto node = std::make_shared<Expr>();
            node->kind = NodeKind::Ternary;
            node->a = condition;
            node->b = thenExpr;
            node->c = elseExpr;
            node->shape = combineShapes(combineShapes(condition->shape, thenExpr->shape, token), elseExpr->shape, token);
            return node;
        }
        return condition;
    }

    template <typename NextLevel>
    std::shared_ptr<Expr> parseLeftAssociative(NextLevel next, const std::vector<std::pair<std::string, BinaryOp>>& operators) {
        auto left = next();
        for (;;) {
            const Token token = current();
            const auto found = std::find_if(operators.begin(), operators.end(), [&](const auto& entry) { return isPunct(entry.first); });
            if (found == operators.end()) return left;
            advance();
            auto right = next();
            auto node = std::make_shared<Expr>();
            node->kind = NodeKind::Binary;
            node->binaryOp = found->second;
            node->a = left;
            node->b = right;
            node->shape = combineShapes(left->shape, right->shape, token);
            left = node;
        }
    }

    std::shared_ptr<Expr> parseLogicalOr() {
        return parseLeftAssociative([this] { return parseLogicalAnd(); }, {{"||", BinaryOp::Or}});
    }
    std::shared_ptr<Expr> parseLogicalAnd() {
        return parseLeftAssociative([this] { return parseEquality(); }, {{"&&", BinaryOp::And}});
    }
    std::shared_ptr<Expr> parseEquality() {
        return parseLeftAssociative([this] { return parseComparison(); }, {{"==", BinaryOp::Equal}, {"!=", BinaryOp::NotEqual}});
    }
    std::shared_ptr<Expr> parseComparison() {
        return parseLeftAssociative([this] { return parseAdditive(); },
            {{"<=", BinaryOp::LessEqual}, {">=", BinaryOp::GreaterEqual}, {"<", BinaryOp::Less}, {">", BinaryOp::Greater}});
    }
    std::shared_ptr<Expr> parseAdditive() {
        return parseLeftAssociative([this] { return parseTerm(); }, {{"+", BinaryOp::Add}, {"-", BinaryOp::Subtract}});
    }
    std::shared_ptr<Expr> parseTerm() {
        return parseLeftAssociative(
            [this] { return parsePower(); }, {{"*", BinaryOp::Multiply}, {"/", BinaryOp::Divide}, {"%", BinaryOp::Modulo}});
    }
    std::shared_ptr<Expr> parsePower() {
        auto left = parseUnary();
        if (isPunct("^")) {
            const auto token = current();
            advance();
            auto right = parsePower();  // right-associative
            auto node = std::make_shared<Expr>();
            node->kind = NodeKind::Binary;
            node->binaryOp = BinaryOp::Power;
            node->a = left;
            node->b = right;
            node->shape = combineShapes(left->shape, right->shape, token);
            return node;
        }
        return left;
    }
    std::shared_ptr<Expr> parseUnary() {
        if (isPunct("-") || isPunct("+") || isPunct("!")) {
            const auto token = current();
            advance();
            auto operand = parseUnary();
            auto node = std::make_shared<Expr>();
            node->kind = NodeKind::Unary;
            node->unaryOp = token.text == "-" ? UnaryOp::Negate : token.text == "+" ? UnaryOp::Plus : UnaryOp::Not;
            node->a = operand;
            node->shape = operand->shape;
            return node;
        }
        return parsePrimary();
    }
    std::shared_ptr<Expr> parsePrimary() {
        const auto token = current();
        if (token.kind == TokKind::Number) {
            advance();
            auto node = std::make_shared<Expr>();
            node->kind = NodeKind::Number;
            node->number = token.number;
            node->shape = {};
            return node;
        }
        if (isPunct("(")) {
            advance();
            auto inner = parseExpr();
            expectPunct(")");
            return inner;
        }
        if (token.kind == TokKind::Ident) {
            advance();
            if (isPunct("(")) {
                advance();
                std::vector<std::shared_ptr<Expr>> args;
                if (!isPunct(")")) {
                    args.push_back(parseExpr());
                    while (isPunct(",")) {
                        advance();
                        args.push_back(parseExpr());
                    }
                }
                expectPunct(")");
                return makeCall(token, std::move(args));
            }
            const auto found = shapes_.find(token.text);
            if (found == shapes_.end()) error(token, "unknown identifier '" + token.text + "' (not a source variable or an earlier statement in this script)");
            auto node = std::make_shared<Expr>();
            node->kind = NodeKind::Identifier;
            node->identifier = token.text;
            node->shape = found->second;
            return node;
        }
        error(token, "expected a number, identifier, function call, or '('");
    }

    std::shared_ptr<Expr> makeCall(const Token& nameToken, std::vector<std::shared_ptr<Expr>> args) {
        const auto& arities = functionArity();
        const auto found = arities.find(nameToken.text);
        if (found == arities.end()) error(nameToken, "unknown function '" + nameToken.text + "'");
        if (static_cast<int>(args.size()) != found->second)
            error(nameToken, "'" + nameToken.text + "' expects " + std::to_string(found->second) + " argument(s), got " + std::to_string(args.size()));
        VariableShape shape = args.front()->shape;
        for (std::size_t i = 1; i < args.size(); ++i) shape = combineShapes(shape, args[i]->shape, nameToken);
        auto node = std::make_shared<Expr>();
        node->kind = NodeKind::Call;
        node->functionName = nameToken.text;
        node->args = std::move(args);
        node->shape = shape;
        return node;
    }
};

// ---- evaluation -----------------------------------------------------

std::vector<float> combineElementwise(const std::vector<float>& left, const std::vector<float>& right, const std::function<float(float, float)>& op) {
    if (left.size() == right.size()) {
        std::vector<float> result(left.size());
        for (std::size_t i = 0; i < left.size(); ++i) result[i] = op(left[i], right[i]);
        return result;
    }
    if (left.size() == 1) {
        std::vector<float> result(right.size());
        for (std::size_t i = 0; i < right.size(); ++i) result[i] = op(left[0], right[i]);
        return result;
    }
    if (right.size() == 1) {
        std::vector<float> result(left.size());
        for (std::size_t i = 0; i < left.size(); ++i) result[i] = op(left[i], right[0]);
        return result;
    }
    // Not reachable given parseDerivedVariables' own shape checking already
    // guarantees conformability - guarded rather than assumed, since a
    // caller-supplied `resolve` returning an unexpectedly-sized slice would
    // otherwise silently read/write out of bounds below.
    throw UserError("Internal error: mismatched array lengths while evaluating a derived variable.");
}

float applyUnary(UnaryOp op, float x) {
    switch (op) {
        case UnaryOp::Negate: return -x;
        case UnaryOp::Plus: return x;
        case UnaryOp::Not: return x == 0.0f ? 1.0f : 0.0f;
    }
    return x;
}

float applyBinary(BinaryOp op, float a, float b) {
    switch (op) {
        case BinaryOp::Add: return a + b;
        case BinaryOp::Subtract: return a - b;
        case BinaryOp::Multiply: return a * b;
        case BinaryOp::Divide: return a / b;
        case BinaryOp::Modulo: return std::fmod(a, b);
        case BinaryOp::Power: return std::pow(a, b);
        case BinaryOp::Less: return a < b ? 1.0f : 0.0f;
        case BinaryOp::LessEqual: return a <= b ? 1.0f : 0.0f;
        case BinaryOp::Greater: return a > b ? 1.0f : 0.0f;
        case BinaryOp::GreaterEqual: return a >= b ? 1.0f : 0.0f;
        case BinaryOp::Equal: return a == b ? 1.0f : 0.0f;
        case BinaryOp::NotEqual: return a != b ? 1.0f : 0.0f;
        case BinaryOp::And: return (a != 0.0f && b != 0.0f) ? 1.0f : 0.0f;
        case BinaryOp::Or: return (a != 0.0f || b != 0.0f) ? 1.0f : 0.0f;
    }
    return 0.0f;
}

float applyUnaryFunction(const std::string& name, float x) {
    if (name == "sqrt") return std::sqrt(x);
    if (name == "exp") return std::exp(x);
    if (name == "log") return std::log(x);
    if (name == "log10") return std::log10(x);
    if (name == "abs") return std::abs(x);
    if (name == "sin") return std::sin(x);
    if (name == "cos") return std::cos(x);
    if (name == "tan") return std::tan(x);
    if (name == "asin") return std::asin(x);
    if (name == "acos") return std::acos(x);
    if (name == "atan") return std::atan(x);
    if (name == "floor") return std::floor(x);
    if (name == "ceil") return std::ceil(x);
    if (name == "round") return std::round(x);
    if (name == "sign") return static_cast<float>((x > 0.0f) - (x < 0.0f));
    throw UserError("Internal error: unknown unary function '" + name + "'.");
}

float applyBinaryFunction(const std::string& name, float a, float b) {
    if (name == "pow") return std::pow(a, b);
    if (name == "atan2") return std::atan2(a, b);
    if (name == "min") return std::min(a, b);
    if (name == "max") return std::max(a, b);
    throw UserError("Internal error: unknown binary function '" + name + "'.");
}

std::vector<float> evaluateNode(const Expr& node, int level, const OperandResolver& resolve) {
    switch (node.kind) {
        case NodeKind::Number: return {static_cast<float>(node.number)};
        case NodeKind::Identifier: {
            const int effectiveLevel = node.shape.dimensionName.empty() ? 0 : level;
            return resolve(node.identifier, effectiveLevel);
        }
        case NodeKind::Unary: {
            auto operand = evaluateNode(*node.a, level, resolve);
            for (auto& value : operand) value = applyUnary(node.unaryOp, value);
            return operand;
        }
        case NodeKind::Binary: {
            const auto left = evaluateNode(*node.a, level, resolve);
            const auto right = evaluateNode(*node.b, level, resolve);
            const auto op = node.binaryOp;
            return combineElementwise(left, right, [op](float a, float b) { return applyBinary(op, a, b); });
        }
        case NodeKind::Ternary: {
            const auto condition = evaluateNode(*node.a, level, resolve);
            const auto thenValues = evaluateNode(*node.b, level, resolve);
            const auto elseValues = evaluateNode(*node.c, level, resolve);
            const auto length = std::max({condition.size(), thenValues.size(), elseValues.size()});
            std::vector<float> result(length);
            for (std::size_t i = 0; i < length; ++i) {
                const float c = condition.size() == 1 ? condition[0] : condition[i];
                const float t = thenValues.size() == 1 ? thenValues[0] : thenValues[i];
                const float e = elseValues.size() == 1 ? elseValues[0] : elseValues[i];
                result[i] = c != 0.0f ? t : e;
            }
            return result;
        }
        case NodeKind::Call: {
            std::vector<std::vector<float>> argValues;
            argValues.reserve(node.args.size());
            for (const auto& arg : node.args) argValues.push_back(evaluateNode(*arg, level, resolve));
            if (argValues.size() == 1) {
                std::vector<float> result(argValues[0].size());
                for (std::size_t i = 0; i < argValues[0].size(); ++i) result[i] = applyUnaryFunction(node.functionName, argValues[0][i]);
                return result;
            }
            const auto& name = node.functionName;
            return combineElementwise(argValues[0], argValues[1], [&name](float a, float b) { return applyBinaryFunction(name, a, b); });
        }
    }
    throw UserError("Internal error: unknown derived-variable expression node.");
}

}  // namespace

std::vector<std::string> builtinFunctionNames() {
    std::vector<std::string> names;
    names.reserve(functionArity().size());
    for (const auto& [name, arity] : functionArity()) names.push_back(name);
    return names;
}

std::vector<DerivedVariableDef> parseDerivedVariables(const std::string& script, const std::map<std::string, VariableShape>& sourceShapes) {
    if (script.find_first_not_of(" \t\r\n") == std::string::npos) return {};
    Parser parser(script, sourceShapes);
    return parser.parseScript();
}

VariableShape shapeOf(const DerivedVariableDef& def) { return def.expression->shape; }

std::vector<float> evaluate(const DerivedVariableDef& def, int level, const OperandResolver& resolve) { return evaluateNode(*def.expression, level, resolve); }

}  // namespace wrftools
