#include "core/fx/fx_expr.hpp"

#include "core/log.hpp"

#include <cmath>
#include <cctype>
#include <limits>
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

    FxExpr parseExpr(std::size_t depth) {
        enterDepth();
        FxExpr lhs = parseTerm(depth);
        for (;;) {
            skipWs();
            const char c = peek();
            if (c != '+' && c != '-')
                break;
            ++pos_;
            FxExpr rhs = parseTerm(depth);
            trackNode();
            FxExpr out;
            auto lhsPtr = std::make_shared<FxExpr>(std::move(lhs));
            auto rhsPtr = std::make_shared<FxExpr>(std::move(rhs));
            out.node = FxExpr::Binary{c, std::move(lhsPtr), std::move(rhsPtr)};
            lhs = std::move(out);
        }
        leaveDepth();
        return lhs;
    }

    FxExpr parseTerm(std::size_t depth) {
        FxExpr lhs = parseFactor(depth);
        for (;;) {
            skipWs();
            const char c = peek();
            if (c != '*' && c != '/' && c != '%')
                break;
            ++pos_;
            FxExpr rhs = parseFactor(depth);
            trackNode();
            FxExpr out;
            auto lhsPtr = std::make_shared<FxExpr>(std::move(lhs));
            auto rhsPtr = std::make_shared<FxExpr>(std::move(rhs));
            out.node = FxExpr::Binary{c, std::move(lhsPtr), std::move(rhsPtr)};
            lhs = std::move(out);
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
            if (name != "pow" && name != "min" && name != "max")
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

bool resolveBuiltin(std::string_view name, const FxEvalContext& context, FxScalar& out) noexcept {
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

FxScalar evalNode(const FxExpr& expr, const FxEvalContext& context, FxCompatibilityProfile profile,
                  bool allowPowQuirk);

FxScalar evalBinary(char op, const FxScalar& lhs, const FxScalar& rhs) {
    const bool lhsDouble = std::holds_alternative<double>(lhs);
    const bool rhsDouble = std::holds_alternative<double>(rhs);
    if (lhsDouble || rhsDouble) {
        const double left = toDoubleImpl(lhs);
        const double right = toDoubleImpl(rhs);
        switch (op) {
        case '+':
            return lhsDouble || rhsDouble ? FxScalar{left + right} : FxScalar{lhs};
        case '-':
            return FxScalar{left - right};
        case '*':
            return FxScalar{left * right};
        case '/':
            if (right == 0.0)
                throw std::runtime_error("fx expression division by zero");
            return FxScalar{left / right};
        case '%':
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
    case '+':
        return FxScalar{checkedAdd(left, right)};
    case '-':
        return FxScalar{checkedSub(left, right)};
    case '*':
        return FxScalar{checkedMul(left, right)};
    case '/':
        if (right == 0)
            throw std::runtime_error("fx expression division by zero");
        if (left == std::numeric_limits<std::int64_t>::min() && right == -1)
            throw std::overflow_error("fx expression integer overflow");
        return FxScalar{left / right};
    case '%':
        if (right == 0)
            throw std::runtime_error("fx expression modulo by zero");
        return FxScalar{left % right};
    default:
        throw std::runtime_error("fx expression has unknown binary operator");
    }
}

FxScalar evalCall(const FxExpr::Call& call, const FxEvalContext& context, FxCompatibilityProfile profile,
                  bool allowPowQuirk) {
    if (call.name == "pow") {
        if (call.args.size() != 2)
            throw std::runtime_error("pow() expects 2 arguments");
        if (profile == FxCompatibilityProfile::upstream130 && !allowPowQuirk) {
            dayo::log::warn("fx pow() rejected under upstream130 without quirk allowlist");
            throw std::runtime_error("pow() requires quirk allowlist under upstream130");
        }
        const double base = toDoubleImpl(evalNode(*call.args[0], context, profile, allowPowQuirk));
        const double exp = toDoubleImpl(evalNode(*call.args[1], context, profile, allowPowQuirk));
        return FxScalar{std::pow(base, exp)};
    }
    if (call.name == "min" || call.name == "max") {
        if (call.args.size() < 2)
            throw std::runtime_error(call.name + "() expects at least 2 arguments");
        const bool wantMin = call.name == "min";
        bool hasDouble = false;
        for (const auto& arg : call.args) {
            if (std::holds_alternative<double>(evalNode(*arg, context, profile, allowPowQuirk)))
                hasDouble = true;
        }
        if (hasDouble) {
            double best = toDoubleImpl(evalNode(*call.args[0], context, profile, allowPowQuirk));
            for (std::size_t i = 1; i < call.args.size(); ++i) {
                const double v = toDoubleImpl(evalNode(*call.args[i], context, profile, allowPowQuirk));
                best = wantMin ? std::fmin(best, v) : std::fmax(best, v);
            }
            return FxScalar{best};
        }
        auto asInt = [](const FxScalar& v) -> std::int64_t {
            if (const auto* b = std::get_if<bool>(&v))
                return *b ? 1 : 0;
            return std::get<std::int64_t>(v);
        };
        std::int64_t best = asInt(evalNode(*call.args[0], context, profile, allowPowQuirk));
        for (std::size_t i = 1; i < call.args.size(); ++i) {
            const std::int64_t v = asInt(evalNode(*call.args[i], context, profile, allowPowQuirk));
            best = wantMin ? std::min(best, v) : std::max(best, v);
        }
        return FxScalar{best};
    }
    throw std::runtime_error("fx expression has unknown function: " + call.name);
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
    if (name == "FRAME" || name == "FRAMEINDEX" || name == "SAMPLE" || name == "SAMPLEINDEX")
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

} // namespace

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
