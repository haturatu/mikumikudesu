#include "core/fx/fx_expr.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>

namespace dayo::core::fx {
namespace {

constexpr std::size_t kMaxExprText = 8192;
constexpr std::size_t kMaxExprNodes = 1024;
constexpr std::size_t kMaxExprDepth = 64;

double toDoubleImpl(const FxScalar& value) noexcept {
    if (const auto* i = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*i);
    if (const auto* d = std::get_if<double>(&value))
        return *d;
    return std::get<bool>(value) ? 1.0 : 0.0;
}

bool toBoolImpl(const FxScalar& value) noexcept {
    if (const auto* b = std::get_if<bool>(&value))
        return *b;
    if (const auto* d = std::get_if<double>(&value))
        return *d != 0.0;
    return std::get<std::int64_t>(value) != 0;
}

std::int64_t checkedAdd(std::int64_t lhs, std::int64_t rhs) {
    std::int64_t out{};
    if (__builtin_add_overflow(lhs, rhs, &out))
        throw std::overflow_error("fx expression integer overflow");
    return out;
}

std::int64_t checkedSub(std::int64_t lhs, std::int64_t rhs) {
    std::int64_t out{};
    if (__builtin_sub_overflow(lhs, rhs, &out))
        throw std::overflow_error("fx expression integer overflow");
    return out;
}

std::int64_t checkedMul(std::int64_t lhs, std::int64_t rhs) {
    std::int64_t out{};
    if (__builtin_mul_overflow(lhs, rhs, &out))
        throw std::overflow_error("fx expression integer overflow");
    return out;
}

std::int64_t checkedNeg(std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::min())
        throw std::overflow_error("fx expression integer overflow");
    return -value;
}

class Parser {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    FxExpr run() {
        if (text_.size() > kMaxExprText)
            throw std::runtime_error("fx expression too long");
        skipWs();
        if (pos_ >= text_.size())
            throw std::runtime_error("fx expression is empty");
        FxExpr expr = parseExpr(0);
        skipWs();
        if (pos_ != text_.size())
            throw std::runtime_error("fx expression has trailing characters");
        return expr;
    }

  private:
    void skipWs() noexcept {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            ++pos_;
    }

    [[nodiscard]] char peek() const noexcept {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    void trackNode() {
        if (++nodes_ > kMaxExprNodes)
            throw std::runtime_error("fx expression too complex");
    }

    void enterDepth() {
        if (++depth_ > kMaxExprDepth)
            throw std::runtime_error("fx expression too deeply nested");
    }

    void leaveDepth() noexcept {
        --depth_;
    }

    bool consume(std::string_view token) {
        skipWs();
        if (text_.substr(pos_, token.size()) != token)
            return false;
        pos_ += token.size();
        return true;
    }

    FxExpr makeBinary(FxExpr::BinaryOp op, FxExpr lhs, FxExpr rhs) {
        trackNode();
        FxExpr out;
        out.node =
            FxExpr::Binary{op, std::make_shared<FxExpr>(std::move(lhs)), std::make_shared<FxExpr>(std::move(rhs))};
        return out;
    }

    FxExpr parseExpr(std::size_t depth) {
        enterDepth();
        FxExpr result = parseLogicalOr(depth);
        leaveDepth();
        return result;
    }

    FxExpr parseLogicalOr(std::size_t depth) {
        FxExpr lhs = parseLogicalAnd(depth);
        while (consume("||")) {
            FxExpr rhs = parseLogicalAnd(depth);
            lhs = makeBinary(FxExpr::BinaryOp::logicalOr, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    FxExpr parseLogicalAnd(std::size_t depth) {
        FxExpr lhs = parseEquality(depth);
        while (consume("&&")) {
            FxExpr rhs = parseEquality(depth);
            lhs = makeBinary(FxExpr::BinaryOp::logicalAnd, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    FxExpr parseEquality(std::size_t depth) {
        FxExpr lhs = parseRelational(depth);
        for (;;) {
            if (consume("==")) {
                lhs = makeBinary(FxExpr::BinaryOp::equal, std::move(lhs), parseRelational(depth));
            } else if (consume("!=")) {
                lhs = makeBinary(FxExpr::BinaryOp::notEqual, std::move(lhs), parseRelational(depth));
            } else {
                break;
            }
        }
        return lhs;
    }

    FxExpr parseRelational(std::size_t depth) {
        FxExpr lhs = parseAdditive(depth);
        for (;;) {
            if (consume("<=")) {
                lhs = makeBinary(FxExpr::BinaryOp::lessEqual, std::move(lhs), parseAdditive(depth));
            } else if (consume(">=")) {
                lhs = makeBinary(FxExpr::BinaryOp::greaterEqual, std::move(lhs), parseAdditive(depth));
            } else if (consume("<")) {
                lhs = makeBinary(FxExpr::BinaryOp::less, std::move(lhs), parseAdditive(depth));
            } else if (consume(">")) {
                lhs = makeBinary(FxExpr::BinaryOp::greater, std::move(lhs), parseAdditive(depth));
            } else {
                break;
            }
        }
        return lhs;
    }

    FxExpr parseAdditive(std::size_t depth) {
        FxExpr lhs = parseMultiplicative(depth);
        for (;;) {
            if (consume("+")) {
                lhs = makeBinary(FxExpr::BinaryOp::add, std::move(lhs), parseMultiplicative(depth));
            } else if (consume("-")) {
                lhs = makeBinary(FxExpr::BinaryOp::subtract, std::move(lhs), parseMultiplicative(depth));
            } else {
                break;
            }
        }
        return lhs;
    }

    FxExpr parseMultiplicative(std::size_t depth) {
        FxExpr lhs = parseFactor(depth);
        for (;;) {
            if (consume("*")) {
                lhs = makeBinary(FxExpr::BinaryOp::multiply, std::move(lhs), parseFactor(depth));
            } else if (consume("/")) {
                lhs = makeBinary(FxExpr::BinaryOp::divide, std::move(lhs), parseFactor(depth));
            } else if (consume("%")) {
                lhs = makeBinary(FxExpr::BinaryOp::modulo, std::move(lhs), parseFactor(depth));
            } else {
                break;
            }
        }
        return lhs;
    }

    FxExpr parseFactor(std::size_t depth) {
        skipWs();
        const char c = peek();
        if (c == '-' || c == '!') {
            ++pos_;
            // Unary operators recurse through parseFactor rather than
            // parseExpr, so they must participate in the same depth budget.
            // Otherwise an untrusted chain such as "!!!!!!!!1" can bypass
            // kMaxExprDepth until the text-size limit is reached.
            enterDepth();
            FxExpr operand = parseFactor(depth);
            leaveDepth();
            trackNode();
            FxExpr out;
            out.node = FxExpr::Unary{c, std::make_shared<FxExpr>(std::move(operand))};
            return out;
        }
        return parsePrimary(depth);
    }

    FxExpr parsePrimary(std::size_t /*depth*/) {
        skipWs();
        const char c = peek();
        if (c == '(') {
            ++pos_;
            FxExpr inner = parseExpr(0);
            skipWs();
            if (peek() != ')')
                throw std::runtime_error("fx expression missing ')'");
            ++pos_;
            return inner;
        }
        if ((std::isdigit(static_cast<unsigned char>(c)) != 0) || c == '.')
            return parseNumber();
        if ((std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_')
            return parseIdentOrCall();
        throw std::runtime_error("fx expression has unexpected character");
    }

    FxExpr parseNumber() {
        const std::size_t begin = pos_;
        bool hasDigits = false;
        while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0)) {
            ++pos_;
            hasDigits = true;
        }
        bool isDouble = false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            // Only treat '.' as decimal point when followed by digit or when
            // we already saw digits (e.g. "1." is valid, but lone "." is not).
            const bool dotFollowedByDigit =
                (pos_ + 1 < text_.size()) && (std::isdigit(static_cast<unsigned char>(text_[pos_ + 1])) != 0);
            if (hasDigits || dotFollowedByDigit) {
                isDouble = true;
                ++pos_;
                while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0)) {
                    ++pos_;
                    hasDigits = true;
                }
            }
        }
        if (!hasDigits)
            throw std::runtime_error("fx expression has invalid number");
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            std::size_t exp = pos_ + 1;
            if (exp < text_.size() && (text_[exp] == '+' || text_[exp] == '-'))
                ++exp;
            if (exp < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[exp])) != 0)) {
                isDouble = true;
                pos_ = exp + 1;
                while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0))
                    ++pos_;
            }
        }
        const std::string_view token = text_.substr(begin, pos_ - begin);
        const std::string tokenStr(token);
        trackNode();
        FxExpr out;
        try {
            if (!isDouble) {
                try {
                    const long long value = std::stoll(tokenStr);
                    out.node = FxExpr::Literal{static_cast<std::int64_t>(value)};
                    return out;
                } catch (const std::out_of_range&) {
                    // Huge integer literals fall back to double; the size
                    // resolver rejects them with overflow instead of wrapping.
                }
            }
            out.node = FxExpr::Literal{std::stod(tokenStr)};
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("fx expression has invalid number");
        } catch (const std::out_of_range&) {
            throw std::runtime_error("fx expression number out of range");
        }
        return out;
    }

    std::string parseIdentText() {
        const std::size_t begin = pos_;
        while (pos_ < text_.size() &&
               ((std::isalnum(static_cast<unsigned char>(text_[pos_])) != 0) || text_[pos_] == '_'))
            ++pos_;
        // Dotted suffixes (DEFAULT_RTSIZE.x). Only consume '.' when it is
        // followed by an identifier start so "a." at end is a syntax error
        // surfaced later rather than silently swallowed.
        while (pos_ < text_.size() && text_[pos_] == '.') {
            const std::size_t dot = pos_;
            ++pos_;
            const std::size_t nameBegin = pos_;
            while (pos_ < text_.size() &&
                   ((std::isalnum(static_cast<unsigned char>(text_[pos_])) != 0) || text_[pos_] == '_'))
                ++pos_;
            if (pos_ == nameBegin) {
                pos_ = dot;
                break;
            }
        }
        return std::string(text_.substr(begin, pos_ - begin));
    }

    FxExpr parseIdentOrCall() {
        const std::string name = parseIdentText();
        if (name.empty())
            throw std::runtime_error("fx expression expects an identifier");
        if (name == "true" || name == "false") {
            trackNode();
            FxExpr out;
            out.node = FxExpr::Literal{name == "true"};
            return out;
        }
        std::size_t saved = pos_;
        skipWs();
        if (peek() == '(') {
            if (!isSupportedFxFunction(name))
                throw std::runtime_error("fx expression has unknown function: " + name);
            ++pos_;
            std::vector<std::shared_ptr<FxExpr>> args;
            skipWs();
            if (peek() != ')') {
                for (;;) {
                    FxExpr arg = parseExpr(0);
                    args.push_back(std::make_shared<FxExpr>(std::move(arg)));
                    if (args.size() > 16)
                        throw std::runtime_error("fx expression has too many call arguments");
                    skipWs();
                    if (peek() == ',') {
                        ++pos_;
                        continue;
                    }
                    break;
                }
            }
            skipWs();
            if (peek() != ')')
                throw std::runtime_error("fx expression missing ')' in call");
            ++pos_;
            trackNode();
            FxExpr out;
            out.node = FxExpr::Call{name, std::move(args)};
            return out;
        }
        pos_ = saved;
        trackNode();
        FxExpr out;
        out.node = FxExpr::Identifier{name};
        return out;
    }

    std::string_view text_;
    std::size_t pos_{};
    std::size_t nodes_{};
    std::size_t depth_{};
};

bool resolveBuiltin(std::string_view name, const FxEvalContext& context, FxScalar& out) {
    if (name == "pi") {
        out = std::numbers::pi_v<double>;
        return true;
    }
    if (name == "Time") {
        out = context.time;
        return true;
    }
    if (const auto found = context.namedSymbols.find(std::string(name)); found != context.namedSymbols.end()) {
        out = found->second;
        return true;
    }
    if (name == "DEFAULT_RTSIZE.x") {
        out = context.rtWidth;
        return true;
    }
    if (name == "DEFAULT_RTSIZE.y") {
        out = context.rtHeight;
        return true;
    }
    if (name == "VERTEXCOUNT") {
        out = context.vertexCount;
        return true;
    }
    if (name == "CLONEDVERTEXCOUNT") {
        out = context.clonedVertexCount;
        return true;
    }
    if (name == "TOTALMATERIAL") {
        out = context.totalMaterial;
        return true;
    }
    if (name == "CloneCount" || name == "CLONECOUNT") {
        out = context.cloneCount;
        return true;
    }
    if (name == "MODELINDEX") {
        out = context.modelIndex;
        return true;
    }
    if (name == "FRAME" || name == "FRAMEINDEX") {
        out = context.frameIndex;
        return true;
    }
    if (name == "SAMPLE" || name == "SAMPLEINDEX") {
        out = context.sampleIndex;
        return true;
    }
    return false;
}

FxScalar evalNode(const FxExpr& expr, const FxEvalContext& context, FxCompatibilityProfile profile, bool allowPowQuirk);

FxScalar evalBinary(FxExpr::BinaryOp op, const FxScalar& lhs, const FxScalar& rhs) {
    const auto comparison = [op](const FxScalar& leftValue, const FxScalar& rightValue) -> std::optional<bool> {
        const bool useDouble = std::holds_alternative<double>(leftValue) || std::holds_alternative<double>(rightValue);
        const auto asInt = [](const FxScalar& value) -> std::int64_t {
            if (const auto* b = std::get_if<bool>(&value))
                return *b ? 1 : 0;
            return std::get<std::int64_t>(value);
        };
        const double leftDouble = toDoubleImpl(leftValue);
        const double rightDouble = toDoubleImpl(rightValue);
        const std::int64_t leftInt = useDouble ? 0 : asInt(leftValue);
        const std::int64_t rightInt = useDouble ? 0 : asInt(rightValue);
        switch (op) {
        case FxExpr::BinaryOp::less:
            return useDouble ? leftDouble < rightDouble : leftInt < rightInt;
        case FxExpr::BinaryOp::lessEqual:
            return useDouble ? leftDouble <= rightDouble : leftInt <= rightInt;
        case FxExpr::BinaryOp::greater:
            return useDouble ? leftDouble > rightDouble : leftInt > rightInt;
        case FxExpr::BinaryOp::greaterEqual:
            return useDouble ? leftDouble >= rightDouble : leftInt >= rightInt;
        case FxExpr::BinaryOp::equal:
            return useDouble ? leftDouble == rightDouble : leftInt == rightInt;
        case FxExpr::BinaryOp::notEqual:
            return useDouble ? leftDouble != rightDouble : leftInt != rightInt;
        default:
            return std::nullopt;
        }
    };
    if (const auto result = comparison(lhs, rhs); result.has_value())
        return FxScalar{*result};
    if (op == FxExpr::BinaryOp::logicalAnd || op == FxExpr::BinaryOp::logicalOr)
        return FxScalar{op == FxExpr::BinaryOp::logicalAnd ? (toBoolImpl(lhs) && toBoolImpl(rhs))
                                                           : (toBoolImpl(lhs) || toBoolImpl(rhs))};

    const bool lhsDouble = std::holds_alternative<double>(lhs);
    const bool rhsDouble = std::holds_alternative<double>(rhs);
    if (lhsDouble || rhsDouble) {
        const double left = toDoubleImpl(lhs);
        const double right = toDoubleImpl(rhs);
        switch (op) {
        case FxExpr::BinaryOp::add:
            return lhsDouble || rhsDouble ? FxScalar{left + right} : FxScalar{lhs};
        case FxExpr::BinaryOp::subtract:
            return FxScalar{left - right};
        case FxExpr::BinaryOp::multiply:
            return FxScalar{left * right};
        case FxExpr::BinaryOp::divide:
            if (right == 0.0)
                throw std::runtime_error("fx expression division by zero");
            return FxScalar{left / right};
        case FxExpr::BinaryOp::modulo:
            if (right == 0.0)
                throw std::runtime_error("fx expression modulo by zero");
            return FxScalar{std::fmod(left, right)};
        default:
            throw std::runtime_error("fx expression has unknown binary operator");
        }
    }
    // Integer path: bools behave as 0/1.
    auto asInt = [](const FxScalar& v) -> std::int64_t {
        if (const auto* b = std::get_if<bool>(&v))
            return *b ? 1 : 0;
        return std::get<std::int64_t>(v);
    };
    const std::int64_t left = asInt(lhs);
    const std::int64_t right = asInt(rhs);
    switch (op) {
    case FxExpr::BinaryOp::add:
        return FxScalar{checkedAdd(left, right)};
    case FxExpr::BinaryOp::subtract:
        return FxScalar{checkedSub(left, right)};
    case FxExpr::BinaryOp::multiply:
        return FxScalar{checkedMul(left, right)};
    case FxExpr::BinaryOp::divide:
        if (right == 0)
            throw std::runtime_error("fx expression division by zero");
        if (left == std::numeric_limits<std::int64_t>::min() && right == -1)
            throw std::overflow_error("fx expression integer overflow");
        return FxScalar{left / right};
    case FxExpr::BinaryOp::modulo:
        if (right == 0)
            throw std::runtime_error("fx expression modulo by zero");
        return FxScalar{left % right};
    default:
        throw std::runtime_error("fx expression has unknown binary operator");
    }
}

FxScalar evalCall(const FxExpr::Call& call, const FxEvalContext& context, FxCompatibilityProfile profile,
                  bool allowPowQuirk) {
    std::vector<FxScalar> arguments;
    arguments.reserve(call.args.size());
    for (const auto& argument : call.args)
        arguments.push_back(evalNode(*argument, context, profile, allowPowQuirk));
    return evaluateFxFunction(call.name, arguments);
}

FxScalar evalNode(const FxExpr& expr, const FxEvalContext& context, FxCompatibilityProfile profile,
                  bool allowPowQuirk) {
    if (const auto* lit = std::get_if<FxExpr::Literal>(&expr.node))
        return lit->value;
    if (const auto* ident = std::get_if<FxExpr::Identifier>(&expr.node)) {
        FxScalar out{};
        if (resolveBuiltin(ident->name, context, out))
            return out;
        dayo::log::warn("fx expression has unknown identifier: ", ident->name);
        throw std::runtime_error("fx expression has unknown identifier: " + ident->name);
    }
    if (const auto* unary = std::get_if<FxExpr::Unary>(&expr.node)) {
        const FxScalar v = evalNode(*unary->operand, context, profile, allowPowQuirk);
        if (unary->op == '!') {
            // NOLINTNEXTLINE(readability-implicit-bool-conversion)
            return FxScalar{!toBoolImpl(v)};
        }
        if (unary->op == '-') {
            if (const auto* d = std::get_if<double>(&v))
                return FxScalar{-(*d)};
            if (const auto* b = std::get_if<bool>(&v))
                return FxScalar{(*b) ? std::int64_t{-1} : std::int64_t{0}};
            return FxScalar{checkedNeg(std::get<std::int64_t>(v))};
        }
        throw std::runtime_error("fx expression has unknown unary operator");
    }
    if (const auto* binary = std::get_if<FxExpr::Binary>(&expr.node)) {
        const FxScalar lhs = evalNode(*binary->lhs, context, profile, allowPowQuirk);
        if (binary->op == FxExpr::BinaryOp::logicalAnd && !toBoolImpl(lhs))
            return FxScalar{false};
        if (binary->op == FxExpr::BinaryOp::logicalOr && toBoolImpl(lhs))
            return FxScalar{true};
        const FxScalar rhs = evalNode(*binary->rhs, context, profile, allowPowQuirk);
        return evalBinary(binary->op, lhs, rhs);
    }
    if (const auto* call = std::get_if<FxExpr::Call>(&expr.node))
        return evalCall(*call, context, profile, allowPowQuirk);
    throw std::runtime_error("fx expression has invalid node");
}

FxExprDependency depOfIdent(std::string_view name) noexcept {
    if (name == "DEFAULT_RTSIZE.x" || name == "DEFAULT_RTSIZE.y")
        return FxExprDependency::Resize;
    if (name == "VERTEXCOUNT" || name == "CLONEDVERTEXCOUNT" || name == "CloneCount" || name == "CLONECOUNT" ||
        name == "MODELINDEX")
        return FxExprDependency::Model;
    if (name == "TOTALMATERIAL")
        return FxExprDependency::Material;
    if (name == "Time" || name == "FRAME" || name == "FRAMEINDEX" || name == "SAMPLE" || name == "SAMPLEINDEX")
        return FxExprDependency::Frame;
    return FxExprDependency::Static;
}

FxExprDependency depOf(const FxExpr& expr) noexcept {
    if (std::holds_alternative<FxExpr::Literal>(expr.node))
        return FxExprDependency::Static;
    if (const auto* ident = std::get_if<FxExpr::Identifier>(&expr.node))
        return depOfIdent(ident->name);
    if (const auto* unary = std::get_if<FxExpr::Unary>(&expr.node)) {
        if (unary->operand != nullptr)
            return depOf(*unary->operand);
        return FxExprDependency::Static;
    }
    if (const auto* binary = std::get_if<FxExpr::Binary>(&expr.node)) {
        FxExprDependency mask = FxExprDependency::Static;
        if (binary->lhs != nullptr)
            mask |= depOf(*binary->lhs);
        if (binary->rhs != nullptr)
            mask |= depOf(*binary->rhs);
        return mask;
    }
    if (const auto* call = std::get_if<FxExpr::Call>(&expr.node)) {
        FxExprDependency mask = FxExprDependency::Static;
        for (const auto& arg : call->args) {
            if (arg != nullptr)
                mask |= depOf(*arg);
        }
        return mask;
    }
    return FxExprDependency::Static;
}

constexpr std::array<std::string_view, 42> kSupportedFunctions{
    "sin",  "cos",        "tan",  "asin", "acos",    "atan",    "atan2",  "sinh",  "cosh",  "tanh",     "exp",
    "log",  "sqrt",       "exp2", "log2", "log10",   "pow",     "abs",    "floor", "ceil",  "trunc",    "round",
    "frac", "fmod",       "mod",  "sign", "degrees", "radians", "min",    "max",   "clamp", "saturate", "lerp",
    "step", "smoothstep", "bit",  "hsvR", "hsvG",    "hsvB",    "select", "hash",  "noise"};

std::uint32_t hashSeed(double input) {
    const double value = std::trunc(static_cast<float>(input));
    double wrapped = std::fmod(value, 4294967296.0);
    if (wrapped < 0.0)
        wrapped += 4294967296.0;
    return static_cast<std::uint32_t>(wrapped);
}

double hashValue(double input) {
    std::uint32_t x = hashSeed(input);
    std::uint32_t y = 114U;
    std::uint32_t z = 514U;
    const auto pcgStep = [](std::uint32_t value) { return value * 1664525U + 1013904223U; };
    x = pcgStep(x);
    y = pcgStep(y);
    z = pcgStep(z);
    x += y * z;
    y += z * x;
    z += x * y;
    x ^= x >> 16U;
    y ^= y >> 16U;
    z ^= z >> 16U;
    x += y * z;
    y += z * x;
    z += x * y;
    return static_cast<double>(x) / 4294967296.0;
}

FxScalar evaluateFunctionImpl(std::string_view name, std::span<const FxScalar> arguments) {
    const auto value = [&arguments](std::size_t index) { return toDoubleImpl(arguments[index]); };
    const auto require = [name, &arguments](std::size_t count) {
        if (arguments.size() != count)
            throw std::runtime_error(std::string(name) + "() expects " + std::to_string(count) + " arguments");
    };
    if (name == "min" || name == "max") {
        if (arguments.size() < 2)
            throw std::runtime_error(std::string(name) + "() expects at least 2 arguments");
        double result = value(0);
        for (std::size_t index = 1; index < arguments.size(); ++index)
            result = name == "min" ? std::fmin(result, value(index)) : std::fmax(result, value(index));
        return FxScalar{result};
    }
    if (name == "sin" || name == "cos" || name == "tan" || name == "asin" || name == "acos" || name == "atan" ||
        name == "sinh" || name == "cosh" || name == "tanh" || name == "exp" || name == "log" || name == "sqrt" ||
        name == "exp2" || name == "log2" || name == "log10" || name == "abs" || name == "floor" || name == "ceil" ||
        name == "trunc" || name == "round" || name == "frac" || name == "sign" || name == "degrees" ||
        name == "radians" || name == "saturate" || name == "hash" || name == "noise") {
        require(1);
        const double input = value(0);
        if (name == "sin")
            return FxScalar{std::sin(input)};
        if (name == "cos")
            return FxScalar{std::cos(input)};
        if (name == "tan")
            return FxScalar{std::tan(input)};
        if (name == "asin")
            return FxScalar{std::asin(input)};
        if (name == "acos")
            return FxScalar{std::acos(input)};
        if (name == "atan")
            return FxScalar{std::atan(input)};
        if (name == "sinh")
            return FxScalar{std::sinh(input)};
        if (name == "cosh")
            return FxScalar{std::cosh(input)};
        if (name == "tanh")
            return FxScalar{std::tanh(input)};
        if (name == "exp")
            return FxScalar{std::exp(input)};
        if (name == "log")
            return FxScalar{std::log(input)};
        if (name == "sqrt")
            return FxScalar{std::sqrt(input)};
        if (name == "exp2")
            return FxScalar{std::exp2(input)};
        if (name == "log2")
            return FxScalar{std::log2(input)};
        if (name == "log10")
            return FxScalar{std::log10(input)};
        if (name == "abs")
            return FxScalar{std::fabs(input)};
        if (name == "floor")
            return FxScalar{std::floor(input)};
        if (name == "ceil")
            return FxScalar{std::ceil(input)};
        if (name == "trunc")
            return FxScalar{std::trunc(input)};
        if (name == "round")
            return FxScalar{std::round(input)};
        if (name == "frac")
            return FxScalar{input - std::floor(input)};
        if (name == "sign")
            return FxScalar{input == 0.0 ? 0.0 : (input > 0.0 ? 1.0 : -1.0)};
        if (name == "degrees")
            return FxScalar{input * 180.0 / std::numbers::pi_v<double>};
        if (name == "radians")
            return FxScalar{input * std::numbers::pi_v<double> / 180.0};
        if (name == "saturate")
            return FxScalar{std::clamp(input, 0.0, 1.0)};
        if (name == "hash")
            return FxScalar{hashValue(input)};
        const double fraction = input - std::floor(input);
        const double integer = input - fraction;
        const double blend = fraction * fraction * fraction * (fraction * (fraction * 6.0 - 15.0) + 10.0);
        return FxScalar{hashValue(integer) * (1.0 - blend) + hashValue(integer + 1.0) * blend};
    }
    if (name == "pow" || name == "atan2" || name == "fmod" || name == "mod" || name == "step" || name == "bit") {
        require(2);
        const double lhs = value(0);
        const double rhs = value(1);
        if (name == "pow")
            return FxScalar{std::pow(lhs, rhs)};
        if (name == "atan2")
            return FxScalar{std::atan2(lhs, rhs)};
        if (name == "fmod")
            return FxScalar{std::fmod(lhs, rhs)};
        if (name == "mod") {
            if (rhs == 0.0)
                throw std::runtime_error("mod() divisor is zero");
            return FxScalar{lhs - rhs * std::floor(lhs / rhs)};
        }
        if (name == "step")
            return FxScalar{lhs >= rhs};
        if (!std::isfinite(lhs) || !std::isfinite(rhs) || lhs < std::numeric_limits<std::int32_t>::min() ||
            lhs > std::numeric_limits<std::int32_t>::max() || rhs < 0.0 || rhs >= 32.0)
            throw std::runtime_error("bit() argument is out of range");
        const auto bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(lhs));
        const auto shift = static_cast<std::uint32_t>(rhs);
        return FxScalar{static_cast<double>((bits >> shift) & 1U)};
    }
    if (name == "clamp" || name == "lerp" || name == "select" || name == "smoothstep") {
        require(3);
        const double first = value(0);
        const double second = value(1);
        const double third = value(2);
        if (name == "clamp")
            return FxScalar{std::max(std::min(third, first), second)};
        if (name == "lerp")
            return FxScalar{first * (1.0 - third) + second * third};
        if (name == "select")
            return FxScalar{fxToBool(arguments[0]) ? second : third};
        if (first == second)
            return FxScalar{first <= third ? 0.0 : 1.0};
        const double u = (third - first) / (second - first);
        const double x = std::max(0.0, std::min(u, 1.0));
        return FxScalar{x * x * (3.0 - 2.0 * x)};
    }
    if (name == "hsvR" || name == "hsvG" || name == "hsvB") {
        require(3);
        double hue = value(0);
        const double saturation = std::clamp(value(1), 0.0, 1.0);
        const double brightness = value(2);
        if (name == "hsvG")
            hue += 2.0 / 3.0;
        else if (name == "hsvB")
            hue += 1.0 / 3.0;
        const double component = std::fabs((hue - std::floor(hue)) * 2.0 - 1.0) * 3.0 - 1.0;
        const double sat = std::clamp(component, 0.0, 1.0);
        return FxScalar{((sat - 1.0) * saturation + 1.0) * brightness};
    }
    throw std::runtime_error("fx expression has unknown function: " + std::string(name));
}

} // namespace

bool isSupportedFxFunction(std::string_view name) noexcept {
    return std::ranges::find(kSupportedFunctions, name) != kSupportedFunctions.end();
}

FxScalar evaluateFxFunction(std::string_view name, std::span<const FxScalar> arguments) {
    if (!isSupportedFxFunction(name))
        throw std::runtime_error("fx expression has unknown function: " + std::string(name));
    return evaluateFunctionImpl(name, arguments);
}

FxExpr parseFxExpr(std::string_view text) {
    Parser parser(text);
    return parser.run();
}

FxScalar evaluateFxExpr(const FxExpr& expr, const FxEvalContext& context, FxCompatibilityProfile profile,
                        bool allowPowQuirk) {
    return evalNode(expr, context, profile, allowPowQuirk);
}

FxExprDependency fxDependencies(const FxExpr& expr) noexcept {
    return depOf(expr);
}

double fxToDouble(const FxScalar& value) noexcept {
    return toDoubleImpl(value);
}

std::int64_t fxToInt(const FxScalar& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value))
        return *i;
    if (const auto* b = std::get_if<bool>(&value))
        return *b ? 1 : 0;
    const double d = std::get<double>(value);
    if (!std::isfinite(d))
        throw std::overflow_error("fx expression value is not finite");
    if (d > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
        d < static_cast<double>(std::numeric_limits<std::int64_t>::min()))
        throw std::overflow_error("fx expression value out of range");
    return static_cast<std::int64_t>(d);
}

bool fxToBool(const FxScalar& value) noexcept {
    return toBoolImpl(value);
}

} // namespace dayo::core::fx
