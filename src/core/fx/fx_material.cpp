#include "core/fx/fx_material.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_set>

namespace dayo::core::fx {
namespace {

std::string trimCopy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string toLower(std::string value) {
    for (auto& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string normalizeTag(std::string_view tag) {
    return toLower(trimCopy(tag));
}

std::string canonicalTagSet(const std::vector<std::string>& tags) {
    std::vector<std::string> normalized;
    normalized.reserve(tags.size());
    for (const auto& tag : tags) {
        auto key = normalizeTag(tag);
        if (!key.empty())
            normalized.push_back(std::move(key));
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    std::string out;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (i != 0)
            out += '+';
        out += normalized[i];
    }
    return out;
}

std::string materialIdentifier(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(byte) || character == '_' ? character : '_');
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front())))
        result.insert(result.begin(), '_');
    return result;
}

std::uint32_t parseMaterialIndex(std::string_view text, std::string_view token) {
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid upstream material declaration: " + std::string(token));
    return value;
}

std::int32_t parseMaterialInteger(std::string_view text, std::string_view description) {
    std::int32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid upstream material integer in " + std::string(description) + ": " +
                                    std::string(text));
    return value;
}

float parseMaterialFloat(std::string_view text, std::string_view description) {
    float value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid upstream material float in " + std::string(description) + ": " +
                                    std::string(text));
    return value;
}

std::vector<std::string> splitMaterialList(std::string_view text) {
    std::vector<std::string> values;
    std::size_t start = 0;
    std::size_t depth = 0;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        const bool atEnd = index == text.size();
        const char character = atEnd ? '\0' : text[index];
        if (quote != '\0') {
            if (!escaped && character == quote)
                quote = '\0';
            escaped = !escaped && character == '\\';
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '(') {
            ++depth;
        } else if (character == ')') {
            if (depth == 0)
                throw std::invalid_argument("unbalanced parentheses in upstream material value list");
            --depth;
        }
        if (!atEnd && (character != ',' || quote != '\0' || depth != 0))
            continue;
        if (quote != '\0' || depth != 0)
            throw std::invalid_argument("unterminated expression in upstream material value list");
        const auto value = trimCopy(text.substr(start, index - start));
        if (value.empty())
            throw std::invalid_argument("empty item in upstream material value list");
        values.push_back(value);
        start = index + 1;
    }
    return values;
}

std::string_view stripMaterialComment(std::string_view line) noexcept {
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (quote != '\0') {
            if (!escaped && character == quote)
                quote = '\0';
            escaped = !escaped && character == '\\';
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '#') {
            return line.substr(0, index);
        }
    }
    return line;
}

std::string unquoteMaterialTexture(std::string_view token) {
    auto value = trimCopy(token);
    const bool startsQuoted = !value.empty() && (value.front() == '\'' || value.front() == '"');
    const bool endsQuoted = !value.empty() && (value.back() == '\'' || value.back() == '"');
    if (startsQuoted != endsQuoted || (startsQuoted && value.back() != value.front()))
        throw std::invalid_argument("malformed quoted upstream material texture path: " + value);
    if (startsQuoted) {
        std::string unquoted;
        unquoted.reserve(value.size() - 2);
        bool escaped = false;
        for (std::size_t index = 1; index + 1 < value.size(); ++index) {
            const char character = value[index];
            if (escaped) {
                unquoted.push_back(character);
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else {
                unquoted.push_back(character);
            }
        }
        if (escaped)
            unquoted.push_back('\\');
        return unquoted;
    }
    return value;
}

void parseMaterialEnum(MaterialTemplateSchema& schema, std::string_view left, std::string_view right,
                       std::size_t lineNumber) {
    std::string fieldName;
    std::string_view valueList = right;
    if (left.size() > 2) {
        fieldName = trimCopy(left.substr(2));
    } else {
        const auto separator = right.find(':');
        if (separator == std::string_view::npos)
            throw std::invalid_argument("invalid upstream material enum declaration at line " +
                                        std::to_string(lineNumber));
        fieldName = trimCopy(right.substr(0, separator));
        valueList = right.substr(separator + 1);
    }
    if (fieldName.empty())
        throw std::invalid_argument("upstream material enum has no field name at line " + std::to_string(lineNumber));

    MaterialEnumSchema enumeration;
    enumeration.field = materialIdentifier(fieldName);
    if (std::ranges::any_of(schema.enums,
                            [&enumeration](const auto& existing) { return existing.field == enumeration.field; }))
        throw std::invalid_argument("duplicate upstream material enum field '" + enumeration.field + "' at line " +
                                    std::to_string(lineNumber));
    std::int32_t nextValue = 0;
    const auto items = splitMaterialList(valueList);
    for (const auto& item : items) {
        const auto assignment = item.find('=');
        const auto name = trimCopy(std::string_view(item).substr(0, assignment));
        if (name.empty())
            throw std::invalid_argument("upstream material enum has an empty label at line " +
                                        std::to_string(lineNumber));
        std::int32_t value = nextValue;
        if (assignment != std::string::npos) {
            value = parseMaterialInteger(trimCopy(std::string_view(item).substr(assignment + 1)), item);
        } else if (nextValue == std::numeric_limits<std::int32_t>::max()) {
            throw std::invalid_argument("upstream material enum value overflows at line " + std::to_string(lineNumber));
        }
        enumeration.values.push_back({.name = name, .value = value});
        if (value == std::numeric_limits<std::int32_t>::max())
            nextValue = value;
        else
            nextValue = value + 1;
    }
    schema.enums.push_back(std::move(enumeration));
}

const MaterialFieldSchema* findMaterialField(const MaterialTemplateSchema& schema, std::string_view name) {
    const auto found = std::find_if(schema.fields.begin(), schema.fields.end(),
                                    [name](const auto& field) { return field.name == name; });
    return found == schema.fields.end() ? nullptr : &*found;
}

std::int32_t parseMaterialEnumValue(const MaterialTemplateSchema& schema, std::string_view field,
                                    std::string_view token) {
    std::int32_t value{};
    const auto numeric = std::from_chars(token.data(), token.data() + token.size(), value);
    if (numeric.ec == std::errc{} && numeric.ptr == token.data() + token.size())
        return value;
    for (const auto& enumeration : schema.enums) {
        if (enumeration.field != field)
            continue;
        const auto found = std::find_if(enumeration.values.begin(), enumeration.values.end(),
                                        [token](const auto& item) { return item.name == token; });
        if (found != enumeration.values.end())
            return found->value;
    }
    throw std::invalid_argument("unknown upstream material enum value for " + std::string(field) + ": " +
                                std::string(token));
}

template <typename T, std::size_t N, typename Parser>
std::array<T, N> parseMaterialArray(const std::vector<std::string>& values, std::string_view description,
                                    Parser parser) {
    if (values.size() != N)
        throw std::invalid_argument("upstream material component count mismatch for " + std::string(description));
    std::array<T, N> result{};
    for (std::size_t index = 0; index < N; ++index)
        result[index] = parser(values[index]);
    return result;
}

void setMaterialValue(MaterialTemplateSchema& schema, const MaterialFieldSchema& field,
                      const std::vector<std::string>& components) {
    if (components.size() != field.components)
        throw std::invalid_argument("upstream material component count mismatch for " + field.name);
    if (field.type == MaterialFieldType::floatingPoint) {
        const auto parse = [&field](std::string_view token) { return parseMaterialFloat(token, field.name); };
        switch (field.components) {
        case 1:
            schema.defaults.set(field.name, parse(components[0]));
            break;
        case 2:
            schema.defaults.set(field.name, parseMaterialArray<float, 2>(components, field.name, parse));
            break;
        case 3:
            schema.defaults.set(field.name, parseMaterialArray<float, 3>(components, field.name, parse));
            break;
        case 4:
            schema.defaults.set(field.name, parseMaterialArray<float, 4>(components, field.name, parse));
            break;
        default:
            throw std::invalid_argument("unsupported upstream material field width for " + field.name);
        }
        return;
    }

    const auto parse = [&schema, &field](std::string_view token) {
        return parseMaterialEnumValue(schema, field.name, token);
    };
    switch (field.components) {
    case 1:
        schema.defaults.set(field.name, parse(components[0]));
        break;
    case 2:
        schema.defaults.set(field.name, parseMaterialArray<std::int32_t, 2>(components, field.name, parse));
        break;
    case 3:
        schema.defaults.set(field.name, parseMaterialArray<std::int32_t, 3>(components, field.name, parse));
        break;
    case 4:
        schema.defaults.set(field.name, parseMaterialArray<std::int32_t, 4>(components, field.name, parse));
        break;
    default:
        throw std::invalid_argument("unsupported upstream material field width for " + field.name);
    }
}

const MaterialTextureSchema* findMaterialTexture(const MaterialTemplateSchema& schema, std::string_view name) {
    const auto found = std::find_if(schema.textures.begin(), schema.textures.end(),
                                    [name](const auto& texture) { return texture.name == name; });
    return found == schema.textures.end() ? nullptr : &*found;
}

MaterialTextureAssignment parseMaterialTextureAssignment(const MaterialTemplateSchema& schema, std::string_view left,
                                                         std::string_view right,
                                                         const std::filesystem::path& baseDirectory) {
    const bool isVolume = left.starts_with("_V");
    const auto field = materialIdentifier(trimCopy(left.substr(2)));
    const auto* texture = findMaterialTexture(schema, field);
    if (texture == nullptr)
        throw std::invalid_argument("unknown upstream material texture assignment: " + field);
    const auto dimension = isVolume ? MaterialTextureDimension::threeD : MaterialTextureDimension::twoD;
    if (texture->dimension != dimension)
        throw std::invalid_argument("upstream material texture assignment dimension mismatch for " + field);
    return {.field = field,
            .dimension = texture->dimension,
            .index = texture->index,
            .mipmapped = texture->mipmapped,
            .path = unquoteMaterialTexture(right),
            .baseDirectory = baseDirectory};
}

MaterialValueExpression parseMaterialValueExpression(const MaterialTemplateSchema& schema,
                                                     const MaterialFieldSchema& field, std::string_view source) {
    const auto components = splitMaterialList(source);
    if (components.size() != field.components)
        throw std::invalid_argument("upstream material component count mismatch for " + field.name);
    MaterialValueExpression result{.field = field.name, .source = trimCopy(source), .components = {}};
    result.components.reserve(components.size());
    for (const auto& component : components) {
        if (field.type == MaterialFieldType::signedInteger) {
            const auto enumeration = std::find_if(schema.enums.begin(), schema.enums.end(),
                                                  [&field](const auto& item) { return item.field == field.name; });
            if (enumeration != schema.enums.end()) {
                const auto label = std::find_if(enumeration->values.begin(), enumeration->values.end(),
                                                [&component](const auto& item) { return item.name == component; });
                if (label != enumeration->values.end()) {
                    FxExpr literal;
                    literal.node = FxExpr::Literal{FxScalar{static_cast<std::int64_t>(label->value)}};
                    result.components.push_back(std::move(literal));
                    continue;
                }
            }
        }
        result.components.push_back(parseFxExpr(component));
    }
    return result;
}

void appendOrReplace(std::vector<MaterialValueExpression>& expressions, MaterialValueExpression value) {
    const auto found = std::find_if(expressions.begin(), expressions.end(),
                                    [&value](const auto& existing) { return existing.field == value.field; });
    if (found == expressions.end())
        expressions.push_back(std::move(value));
    else
        *found = std::move(value);
}

void appendOrReplace(std::vector<MaterialTextureAssignment>& assignments, MaterialTextureAssignment value) {
    const auto found = std::find_if(assignments.begin(), assignments.end(),
                                    [&value](const auto& existing) { return existing.field == value.field; });
    if (found == assignments.end())
        assignments.push_back(std::move(value));
    else
        *found = std::move(value);
}

void overlayTemplateDefaults(MaterialTemplateSchema& schema, std::string_view source) {
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        const auto lineEnd = source.find('\n', lineStart);
        const auto length = lineEnd == std::string_view::npos ? source.size() - lineStart : lineEnd - lineStart;
        auto line = source.substr(lineStart, length);
        if (lineStart == 0 && line.starts_with("\xEF\xBB\xBF"))
            line.remove_prefix(3);
        line = stripMaterialComment(line);
        const auto separator = line.find(':');
        if (separator != std::string_view::npos) {
            const auto left = trimCopy(line.substr(0, separator));
            const auto fieldName = materialIdentifier(left);
            const auto* field = findMaterialField(schema, fieldName);
            const auto values = trimCopy(line.substr(separator + 1));
            if (field != nullptr && !values.empty()) {
                setMaterialValue(schema, *field, splitMaterialList(values));
            } else if ((left.starts_with("_T") || left.starts_with("_V")) && left.size() > 2 &&
                       !std::isdigit(static_cast<unsigned char>(left[2])) && !values.empty()) {
                auto assignment =
                    parseMaterialTextureAssignment(schema, left, values, schema.templateTextureBaseDirectory);
                appendOrReplace(schema.templateTextureAssignments, std::move(assignment));
            }
        }
        if (lineEnd == std::string_view::npos)
            break;
        lineStart = lineEnd + 1;
    }
}

bool materialValueMatches(const MaterialFieldSchema& field, const MaterialValue& value) {
    if (field.type == MaterialFieldType::floatingPoint) {
        switch (field.components) {
        case 1:
            return std::holds_alternative<float>(value);
        case 2:
            return std::holds_alternative<std::array<float, 2>>(value);
        case 3:
            return std::holds_alternative<std::array<float, 3>>(value);
        case 4:
            return std::holds_alternative<std::array<float, 4>>(value);
        default:
            return false;
        }
    }
    switch (field.components) {
    case 1:
        return std::holds_alternative<std::int32_t>(value);
    case 2:
        return std::holds_alternative<std::array<std::int32_t, 2>>(value);
    case 3:
        return std::holds_alternative<std::array<std::int32_t, 3>>(value);
    case 4:
        return std::holds_alternative<std::array<std::int32_t, 4>>(value);
    default:
        return false;
    }
}

FxExpr materialLiteral(FxScalar value) {
    FxExpr expression;
    expression.node = FxExpr::Literal{value};
    return expression;
}

std::vector<FxExpr> materialValueExpressions(const MaterialFieldSchema& field, const MaterialValue& value) {
    if (!materialValueMatches(field, value))
        throw std::invalid_argument("FX material value type does not match schema field: " + field.name);
    std::vector<FxExpr> result;
    result.reserve(field.components);
    const auto append = [&result](const auto& component) {
        using Component = std::remove_cvref_t<decltype(component)>;
        if constexpr (std::is_same_v<Component, float>)
            result.push_back(materialLiteral(FxScalar{static_cast<double>(component)}));
        else if constexpr (std::is_same_v<Component, std::int32_t>)
            result.push_back(materialLiteral(FxScalar{static_cast<std::int64_t>(component)}));
    };
    std::visit(
        [&append](const auto& typed) {
            using Value = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, float> || std::is_same_v<Value, std::int32_t>) {
                append(typed);
            } else if constexpr (std::is_same_v<Value, std::array<float, 2>> ||
                                 std::is_same_v<Value, std::array<float, 3>> ||
                                 std::is_same_v<Value, std::array<float, 4>> ||
                                 std::is_same_v<Value, std::array<std::int32_t, 2>> ||
                                 std::is_same_v<Value, std::array<std::int32_t, 3>> ||
                                 std::is_same_v<Value, std::array<std::int32_t, 4>>) {
                for (const auto component : typed)
                    append(component);
            }
        },
        value);
    return result;
}

MaterialValue evaluateMaterialExpression(const MaterialBindingPlan::LinkedField& field, const FxEvalContext& context) {
    const auto& schema = field.schema;
    const auto& components = field.expression.components;
    if (components.size() != schema.components)
        throw std::invalid_argument("FX material expression component count does not match schema field: " +
                                    schema.name);
    std::vector<FxScalar> evaluated;
    evaluated.reserve(components.size());
    for (const auto& component : components)
        evaluated.push_back(evaluateFxExpr(component, context));

    const auto asFloat = [&evaluated](std::size_t index) { return static_cast<float>(fxToDouble(evaluated[index])); };
    const auto asInt = [&evaluated, &schema](std::size_t index) {
        const auto value = fxToInt(evaluated[index]);
        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
            throw std::overflow_error("FX material integer value out of range for " + schema.name);
        return static_cast<std::int32_t>(value);
    };
    if (schema.type == MaterialFieldType::floatingPoint) {
        switch (schema.components) {
        case 1:
            return asFloat(0);
        case 2:
            return std::array<float, 2>{asFloat(0), asFloat(1)};
        case 3:
            return std::array<float, 3>{asFloat(0), asFloat(1), asFloat(2)};
        case 4:
            return std::array<float, 4>{asFloat(0), asFloat(1), asFloat(2), asFloat(3)};
        default:
            break;
        }
    } else {
        switch (schema.components) {
        case 1:
            return asInt(0);
        case 2:
            return std::array<std::int32_t, 2>{asInt(0), asInt(1)};
        case 3:
            return std::array<std::int32_t, 3>{asInt(0), asInt(1), asInt(2)};
        case 4:
            return std::array<std::int32_t, 4>{asInt(0), asInt(1), asInt(2), asInt(3)};
        default:
            break;
        }
    }
    throw std::invalid_argument("unsupported FX material field width for " + schema.name);
}

MaterialAnnotation parseMaterialAnnotationImpl(const MaterialTemplateSchema& schema, std::string_view source,
                                               const std::filesystem::path& baseDirectory) {
    MaterialAnnotation annotation;
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        const auto lineEnd = source.find('\n', lineStart);
        const auto length = lineEnd == std::string_view::npos ? source.size() - lineStart : lineEnd - lineStart;
        auto line = source.substr(lineStart, length);
        if (lineStart == 0 && line.starts_with("\xEF\xBB\xBF"))
            line.remove_prefix(3);
        line = stripMaterialComment(line);
        const auto separator = line.find(':');
        if (separator != std::string_view::npos) {
            const auto left = trimCopy(line.substr(0, separator));
            const auto values = trimCopy(line.substr(separator + 1));
            if (!left.empty() && !values.empty()) {
                const auto fieldName = materialIdentifier(left);
                if (const auto* field = findMaterialField(schema, fieldName); field != nullptr) {
                    appendOrReplace(annotation.values, parseMaterialValueExpression(schema, *field, values));
                } else if ((left.starts_with("_T") || left.starts_with("_V")) && left.size() > 2 &&
                           !std::isdigit(static_cast<unsigned char>(left[2]))) {
                    appendOrReplace(annotation.textures,
                                    parseMaterialTextureAssignment(schema, left, values, baseDirectory));
                }
            }
        }
        if (lineEnd == std::string_view::npos)
            break;
        lineStart = lineEnd + 1;
    }
    return annotation;
}

std::string resolveImpl(std::string_view id, const std::unordered_map<std::string, MaterialResourceDecl>& byId,
                        std::vector<std::string>& stack) {
    const std::string key = trimCopy(id);
    if (key.empty())
        return {};
    const auto found = byId.find(key);
    if (found == byId.end()) {
        // Dangling ref target: keep the trimmed name as canonical.
        return key;
    }
    const MaterialResourceDecl& decl = found->second;
    const std::string ref = trimCopy(decl.ref);
    if (!ref.empty()) {
        const auto cycleStart = std::find(stack.begin(), stack.end(), ref);
        if (cycleStart != stack.end()) {
            // Alias cycle: deterministic fallback to the smallest id in the cycle.
            std::string smallest = ref;
            for (auto it = cycleStart; it != stack.end(); ++it)
                smallest = std::min(smallest, *it);
            smallest = std::min(smallest, key);
            log::warn("fx material alias cycle at '", key, "' -> '", smallest, "'");
            return smallest;
        }
        stack.push_back(key);
        const std::string resolved = resolveImpl(ref, byId, stack);
        stack.pop_back();
        return resolved;
    }
    const std::string tags = canonicalTagSet(decl.shareTags);
    if (!tags.empty())
        return "tag:" + tags;
    if (decl.shared)
        return "shared:" + key;
    return key;
}

void storeMaterialWord(std::byte* destination, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        destination[index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void storeMaterialFloat(std::byte* destination, float value) noexcept {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    storeMaterialWord(destination, std::bit_cast<std::uint32_t>(value));
}

void storeMaterialInteger(std::byte* destination, std::int32_t value) noexcept {
    storeMaterialWord(destination, static_cast<std::uint32_t>(value));
}

void storeMaterialField(std::byte* destination, const MaterialFieldSchema& schema, const MaterialValue& value) {
    if (!materialValueMatches(schema, value))
        throw std::invalid_argument("evaluated FX material value does not match schema field: " + schema.name);
    std::visit(
        [destination](const auto& typed) {
            using Value = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, float>) {
                storeMaterialFloat(destination, typed);
            } else if constexpr (std::is_same_v<Value, std::int32_t>) {
                storeMaterialInteger(destination, typed);
            } else if constexpr (std::is_same_v<Value, std::array<float, 2>> ||
                                 std::is_same_v<Value, std::array<float, 3>> ||
                                 std::is_same_v<Value, std::array<float, 4>>) {
                for (std::size_t index = 0; index < typed.size(); ++index)
                    storeMaterialFloat(destination + index * sizeof(float), typed[index]);
            } else if constexpr (std::is_same_v<Value, std::array<std::int32_t, 2>> ||
                                 std::is_same_v<Value, std::array<std::int32_t, 3>> ||
                                 std::is_same_v<Value, std::array<std::int32_t, 4>>) {
                for (std::size_t index = 0; index < typed.size(); ++index)
                    storeMaterialInteger(destination + index * sizeof(std::int32_t), typed[index]);
            }
        },
        value);
}

} // namespace

MaterialTemplateSchema parseMaterialTemplateSchema(std::string_view source, std::string name) {
    MaterialTemplateSchema result;
    result.name = std::move(name);
    result.sourceText = source;

    std::unordered_set<std::string> fieldNames;
    std::unordered_set<std::string> textureNames;
    std::size_t lineNumber = 0;
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        ++lineNumber;
        const auto lineEnd = source.find('\n', lineStart);
        const auto length = lineEnd == std::string_view::npos ? source.size() - lineStart : lineEnd - lineStart;
        auto line = source.substr(lineStart, length);
        if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF"))
            line.remove_prefix(3);
        line = stripMaterialComment(line);
        const auto separator = line.find(':');
        if (separator != std::string_view::npos) {
            const auto left = trimCopy(line.substr(0, separator));
            const auto right = trimCopy(line.substr(separator + 1));
            if (!left.empty() && !right.empty()) {
                if ((left[0] == 'f' || left[0] == 'i') && left.size() >= 3 && left[1] == '.') {
                    const auto componentCount = parseMaterialIndex(std::string_view(left).substr(2), left);
                    if (componentCount == 0 || componentCount > 4)
                        throw std::invalid_argument("upstream material field component count is out of range at line " +
                                                    std::to_string(lineNumber));
                    auto fieldName = materialIdentifier(right);
                    if (!fieldNames.insert(fieldName).second)
                        throw std::invalid_argument("duplicate upstream material field '" + fieldName + "' at line " +
                                                    std::to_string(lineNumber));
                    result.fields.push_back(
                        {.name = std::move(fieldName),
                         .type = left[0] == 'f' ? MaterialFieldType::floatingPoint : MaterialFieldType::signedInteger,
                         .components = componentCount});
                } else if (left.starts_with("_E")) {
                    parseMaterialEnum(result, left, right, lineNumber);
                } else if ((left.starts_with("_T") || left.starts_with("_V")) && left.size() > 2 &&
                           std::isdigit(static_cast<unsigned char>(left[2]))) {
                    std::size_t digitsEnd = 2;
                    while (digitsEnd < left.size() && std::isdigit(static_cast<unsigned char>(left[digitsEnd])))
                        ++digitsEnd;
                    if (digitsEnd == 2 || (digitsEnd != left.size() && left.substr(digitsEnd) != "m"))
                        throw std::invalid_argument("invalid upstream material texture declaration at line " +
                                                    std::to_string(lineNumber) + ": " + left);
                    const auto index = parseMaterialIndex(std::string_view(left).substr(2, digitsEnd - 2), left);
                    if (index == std::numeric_limits<std::uint32_t>::max())
                        throw std::invalid_argument("upstream material texture index is out of range at line " +
                                                    std::to_string(lineNumber));
                    auto textureName = materialIdentifier(right);
                    if (!textureNames.insert(textureName).second)
                        throw std::invalid_argument("duplicate upstream material texture '" + textureName +
                                                    "' at line " + std::to_string(lineNumber));
                    result.textures.push_back({.name = std::move(textureName),
                                               .dimension = left[1] == 'V' ? MaterialTextureDimension::threeD
                                                                           : MaterialTextureDimension::twoD,
                                               .index = index,
                                               .mipmapped = digitsEnd < left.size()});
                }
            }
        }
        if (lineEnd == std::string_view::npos)
            break;
        lineStart = lineEnd + 1;
    }
    for (const auto& field : result.fields)
        setMaterialValue(result, field, std::vector<std::string>(field.components, "0"));
    overlayTemplateDefaults(result, source);
    return result;
}

MaterialTemplateSchema loadMaterialTemplateSchema(const std::filesystem::path& path, std::string name,
                                                  std::filesystem::path textureBaseDirectory) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open upstream material template: " + path.string());
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad())
        throw std::runtime_error("cannot read upstream material template: " + path.string());
    auto schema = parseMaterialTemplateSchema(source, std::move(name));
    schema.templateTextureBaseDirectory =
        textureBaseDirectory.empty() ? path.parent_path() : std::move(textureBaseDirectory);
    for (auto& assignment : schema.templateTextureAssignments)
        assignment.baseDirectory = schema.templateTextureBaseDirectory;
    return schema;
}

MaterialAnnotation parseMaterialAnnotation(const MaterialTemplateSchema& schema, std::string_view source,
                                           std::filesystem::path baseDirectory) {
    return parseMaterialAnnotationImpl(schema, source, baseDirectory);
}

void applyMaterialDefaultFile(MaterialTemplateSchema& schema, std::string_view source,
                              std::filesystem::path baseDirectory) {
    schema.defaultFileSourceText = source;
    schema.defaultFileBaseDirectory = std::move(baseDirectory);
    schema.defaultFileAnnotation = parseMaterialAnnotation(schema, source, schema.defaultFileBaseDirectory);
}

void loadMaterialDefaultFile(MaterialTemplateSchema& schema, const std::filesystem::path& path) {
    if (path.empty())
        return;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open upstream material default file: " + path.string());
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad())
        throw std::runtime_error("cannot read upstream material default file: " + path.string());
    applyMaterialDefaultFile(schema, source, path.parent_path());
}

std::size_t MaterialTextureKeyHash::operator()(const MaterialTextureKey& key) const noexcept {
    std::size_t seed = std::hash<std::string>{}(key.path);
    seed ^= std::hash<std::string>{}(key.externalId) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<std::string>{}(key.format) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<std::string>{}(key.colorspace) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<std::string>{}(key.mipPolicy) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    seed ^=
        std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.dimension)) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<bool>{}(key.mipmapped) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    return seed;
}

std::string normalizeTexturePath(std::string_view path) {
    std::string raw = trimCopy(path);
    if (raw.empty())
        return {};
    for (auto& ch : raw)
        if (ch == '\\')
            ch = '/';
    // lexically_normal keeps the string form stable without touching disk.
    std::filesystem::path normalized(raw);
    normalized = normalized.lexically_normal();
    std::string out = normalized.generic_string();
    if (out == ".")
        return {};
    return out;
}

std::string normalizeTextureToken(std::string_view token) noexcept {
    try {
        return toLower(trimCopy(token));
    } catch (...) {
        return {};
    }
}

MaterialTextureKey makeTextureKey(const MaterialTextureDesc& desc) {
    return MaterialTextureKey{
        .path = normalizeTexturePath(desc.path),
        .externalId = desc.externalId,
        .format = normalizeTextureToken(desc.format),
        .colorspace = normalizeTextureToken(desc.colorspace),
        .mipPolicy = normalizeTextureToken(desc.mipPolicy),
        .dimension = desc.dimension,
        .mipmapped = desc.mipmapped,
    };
}

std::string textureKeyString(const MaterialTextureKey& key) {
    return key.path + "|" + key.externalId + "|" + key.format + "|" + key.colorspace + "|" + key.mipPolicy + "|" +
           std::to_string(static_cast<std::uint8_t>(key.dimension)) + "|" + (key.mipmapped ? "mipped" : "nomip");
}

std::string MaterialGpuLayout::slotForLocal(std::string_view localId) const {
    auto findSlot = [&](const std::string& key) -> std::string {
        const auto local = localToCanonical.find(key);
        if (local == localToCanonical.end())
            return {};
        const auto slot = canonicalToSlot.find(local->second);
        return slot == canonicalToSlot.end() ? std::string{} : slot->second;
    };
    std::string direct(localId);
    std::string hit = findSlot(direct);
    if (!hit.empty())
        return hit;
    return findSlot(trimCopy(localId));
}

std::string MaterialGpuLayout::slotForCanonical(std::string_view canonical) const {
    const auto slot = canonicalToSlot.find(std::string(canonical));
    return slot == canonicalToSlot.end() ? std::string{} : slot->second;
}

const std::string* MaterialBindingPlan::slotFor(std::string_view localId) const noexcept {
    try {
        const auto findSlot = [&](std::string_view id) -> const std::string* {
            const auto local = layout.localToCanonical.find(std::string(id));
            if (local == layout.localToCanonical.end())
                return nullptr;
            const auto slot = layout.canonicalToSlot.find(local->second);
            return slot == layout.canonicalToSlot.end() ? nullptr : &slot->second;
        };
        if (const auto* slot = findSlot(localId); slot != nullptr)
            return slot;
        const std::string trimmed = trimCopy(localId);
        return findSlot(trimmed);
    } catch (...) {
        return nullptr;
    }
}

bool MaterialGpuLayout::hasLocal(std::string_view localId) const noexcept {
    try {
        return localToCanonical.contains(std::string(localId)) || localToCanonical.contains(trimCopy(localId));
    } catch (...) {
        return false;
    }
}

std::string resolveCanonicalResourceId(std::string_view id,
                                       const std::unordered_map<std::string, MaterialResourceDecl>& byId) {
    std::vector<std::string> stack;
    return resolveImpl(id, byId, stack);
}

MaterialGpuLayout linkMaterialLayout(const MaterialTemplate& templ, const MaterialInstance* instance) {
    std::unordered_map<std::string, MaterialResourceDecl> byId;
    byId.reserve(templ.resources.size() + (instance != nullptr ? instance->extraResources.size() : 0U));
    for (const auto& decl : templ.resources) {
        const std::string key = trimCopy(decl.id);
        if (key.empty()) {
            log::warn("fx material '", templ.name, "' skips resource with empty id");
            continue;
        }
        if (byId.contains(key))
            log::warn("fx material '", templ.name, "' duplicate resource '", key, "' keeps last");
        MaterialResourceDecl copy = decl;
        copy.id = key;
        byId[key] = std::move(copy);
    }
    if (instance != nullptr) {
        for (const auto& decl : instance->extraResources) {
            const std::string key = trimCopy(decl.id);
            if (key.empty()) {
                log::warn("fx material instance '", instance->templateName, "' skips resource with empty id");
                continue;
            }
            MaterialResourceDecl copy = decl;
            copy.id = key;
            byId[key] = std::move(copy);
        }
    }

    MaterialGpuLayout layout;
    // Fold every local id to canonical. Sorting the input makes iteration
    // order irrelevant; canonical outputs are sorted below for _R stability.
    std::vector<std::string> localIds;
    localIds.reserve(byId.size());
    for (const auto& entry : byId)
        localIds.push_back(entry.first);
    std::sort(localIds.begin(), localIds.end());
    for (const auto& id : localIds)
        layout.localToCanonical[id] = resolveCanonicalResourceId(id, byId);

    std::unordered_set<std::string> canonicalSet;
    canonicalSet.reserve(layout.localToCanonical.size() * 2U);
    for (const auto& entry : layout.localToCanonical) {
        if (!entry.second.empty())
            canonicalSet.insert(entry.second);
    }
    layout.canonicalResources.assign(canonicalSet.begin(), canonicalSet.end());
    std::sort(layout.canonicalResources.begin(), layout.canonicalResources.end());
    for (std::size_t i = 0; i < layout.canonicalResources.size(); ++i)
        layout.canonicalToSlot[layout.canonicalResources[i]] = "_R" + std::to_string(i);

    // UniqueTextures: dedup by canonical key, keep normalized representative.
    std::unordered_map<MaterialTextureKey, MaterialTextureDesc, MaterialTextureKeyHash> unique;
    unique.reserve(byId.size());
    for (const auto& id : localIds) {
        const auto& decl = byId.at(id);
        if (!decl.hasTexture)
            continue;
        MaterialTextureDesc normalized{
            .path = normalizeTexturePath(decl.texture.path),
            .externalId = decl.texture.externalId,
            .format = normalizeTextureToken(decl.texture.format),
            .colorspace = normalizeTextureToken(decl.texture.colorspace),
            .mipPolicy = normalizeTextureToken(decl.texture.mipPolicy),
            .dimension = decl.texture.dimension,
            .mipmapped = decl.texture.mipmapped,
        };
        const MaterialTextureKey key{
            .path = normalized.path,
            .externalId = normalized.externalId,
            .format = normalized.format,
            .colorspace = normalized.colorspace,
            .mipPolicy = normalized.mipPolicy,
            .dimension = normalized.dimension,
            .mipmapped = normalized.mipmapped,
        };
        if (!unique.contains(key))
            unique.emplace(key, std::move(normalized));
    }
    layout.uniqueTextures.reserve(unique.size());
    for (auto& entry : unique)
        layout.uniqueTextures.push_back(std::move(entry.second));
    std::sort(layout.uniqueTextures.begin(), layout.uniqueTextures.end(), [](const auto& lhs, const auto& rhs) {
        return textureKeyString(makeTextureKey(lhs)) < textureKeyString(makeTextureKey(rhs));
    });

    log::debug("fx material '", templ.name, "' linked ", layout.canonicalResources.size(), " resources, ",
               layout.uniqueTextures.size(), " unique textures");
    return layout;
}

MaterialBindingPlan linkMaterial(const MaterialTemplate& templ, const MaterialInstance* instance) {
    MaterialBindingPlan plan{.layout = linkMaterialLayout(templ, instance),
                             .resolvedParameters = {},
                             .orderedExpressions = {},
                             .orderedTextures = {}};
    plan.resolvedParameters = templ.defaults;
    if (instance != nullptr) {
        for (const auto& [name, value] : instance->overrides.values())
            plan.resolvedParameters.set(name, value);
    }
    log::debug("fx material '", templ.name, "' binding plan with ", plan.layout.canonicalToSlot.size(), " slots");
    return plan;
}

MaterialBindingPlan linkMaterial(const MaterialTemplateSchema& schema, const MaterialInstance* instance) {
    MaterialTemplate templ;
    templ.name = schema.name;
    templ.defaults = schema.defaults;

    std::vector<MaterialValueExpression> linkedValues;
    linkedValues.reserve(schema.fields.size());
    for (const auto& field : schema.fields) {
        const auto* value = schema.defaults.find(field.name);
        if (value == nullptr)
            throw std::invalid_argument("FX material schema has no default value for field: " + field.name);
        linkedValues.push_back({.field = field.name,
                                .source = "<template default>",
                                .components = materialValueExpressions(field, *value)});
    }
    const auto overlayValues = [&schema, &linkedValues](const MaterialAnnotation& annotation) {
        for (const auto& value : annotation.values) {
            const auto field = std::find_if(schema.fields.begin(), schema.fields.end(),
                                            [&value](const auto& candidate) { return candidate.name == value.field; });
            if (field == schema.fields.end())
                throw std::invalid_argument("unknown FX material annotation field: " + value.field);
            if (value.components.size() != field->components)
                throw std::invalid_argument("FX material annotation component count mismatch for " + value.field);
            appendOrReplace(linkedValues, value);
        }
    };
    overlayValues(schema.defaultFileAnnotation);
    if (instance != nullptr) {
        overlayValues(instance->annotationOverrides);
        for (const auto& [name, value] : instance->overrides.values()) {
            const auto field = std::find_if(schema.fields.begin(), schema.fields.end(),
                                            [&name](const auto& candidate) { return candidate.name == name; });
            if (field == schema.fields.end())
                throw std::invalid_argument("unknown FX material instance override: " + name);
            appendOrReplace(linkedValues,
                            MaterialValueExpression{.field = name,
                                                    .source = "<editor override>",
                                                    .components = materialValueExpressions(*field, value)});
        }
    }

    std::vector<MaterialTextureAssignment> linkedTextures = schema.templateTextureAssignments;
    const auto overlayTextures = [&schema, &linkedTextures](const MaterialAnnotation& annotation) {
        for (const auto& assignment : annotation.textures) {
            const auto texture =
                std::find_if(schema.textures.begin(), schema.textures.end(),
                             [&assignment](const auto& candidate) { return candidate.name == assignment.field; });
            if (texture == schema.textures.end())
                throw std::invalid_argument("unknown FX material texture annotation: " + assignment.field);
            if (texture->dimension != assignment.dimension || texture->index != assignment.index ||
                texture->mipmapped != assignment.mipmapped)
                throw std::invalid_argument("FX material texture annotation does not match schema: " +
                                            assignment.field);
            appendOrReplace(linkedTextures, assignment);
        }
    };
    overlayTextures(schema.defaultFileAnnotation);
    if (instance != nullptr)
        overlayTextures(instance->annotationOverrides);

    struct TextureSource {
        MaterialTextureKey key;
        std::string path;
        std::filesystem::path baseDirectory;
    };
    std::unordered_map<std::string, TextureSource> textureSources;
    std::unordered_map<std::string, MaterialTextureAssignment> assignmentsByField;
    for (const auto& assignment : linkedTextures)
        assignmentsByField[assignment.field] = assignment;

    templ.resources.reserve(schema.textures.size());
    for (const auto& texture : schema.textures) {
        MaterialResourceDecl resource;
        resource.id = texture.name;
        resource.texture.dimension = texture.dimension;
        resource.texture.mipmapped = texture.mipmapped;
        resource.texture.mipPolicy = texture.mipmapped ? "mipmapped" : "none";
        const auto assignment = assignmentsByField.find(texture.name);
        if (assignment != assignmentsByField.end()) {
            const std::filesystem::path assignedPath(assignment->second.path);
            const auto fullPath =
                assignedPath.is_absolute() ? assignedPath : assignment->second.baseDirectory / assignedPath;
            resource.texture.path = normalizeTexturePath(fullPath.generic_string());
            resource.hasTexture = true;
            textureSources.emplace(texture.name, TextureSource{.key = makeTextureKey(resource.texture),
                                                               .path = assignment->second.path,
                                                               .baseDirectory = assignment->second.baseDirectory});
        }
        templ.resources.push_back(std::move(resource));
    }

    if (instance != nullptr) {
        for (const auto& resource : instance->extraResources) {
            if (resource.hasTexture)
                textureSources[resource.id] = TextureSource{
                    .key = makeTextureKey(resource.texture), .path = resource.texture.path, .baseDirectory = {}};
        }
    }

    auto plan = linkMaterial(templ, instance);
    // Values stay as expressions until the caller supplies the invocation context.
    plan.resolvedParameters.clear();
    plan.orderedExpressions.reserve(schema.fields.size());
    for (const auto& field : schema.fields) {
        const auto expression = std::find_if(linkedValues.begin(), linkedValues.end(),
                                             [&field](const auto& value) { return value.field == field.name; });
        if (expression == linkedValues.end())
            throw std::invalid_argument("FX material schema has no linked value expression for field: " + field.name);
        plan.orderedExpressions.push_back({.schema = field, .expression = *expression});
    }

    plan.orderedTextures.reserve(schema.textures.size());
    for (const auto& texture : schema.textures) {
        MaterialBindingPlan::ResolvedTextureField resolved{.schema = texture,
                                                           .canonicalId = {},
                                                           .physicalTextureIndex = std::nullopt,
                                                           .path = {},
                                                           .baseDirectory = {}};
        if (const auto canonical = plan.layout.localToCanonical.find(texture.name);
            canonical != plan.layout.localToCanonical.end())
            resolved.canonicalId = canonical->second;
        if (const auto source = textureSources.find(texture.name); source != textureSources.end()) {
            resolved.path = source->second.path;
            resolved.baseDirectory = source->second.baseDirectory;
            const auto physical = std::find_if(
                plan.layout.uniqueTextures.begin(), plan.layout.uniqueTextures.end(),
                [&source](const auto& candidate) { return makeTextureKey(candidate) == source->second.key; });
            if (physical != plan.layout.uniqueTextures.end())
                resolved.physicalTextureIndex =
                    static_cast<std::size_t>(std::distance(plan.layout.uniqueTextures.begin(), physical));
        }
        plan.orderedTextures.push_back(std::move(resolved));
    }
    return plan;
}

EvaluatedMaterialBinding evaluateMaterialValues(const MaterialBindingPlan& plan, const FxEvalContext& context) {
    EvaluatedMaterialBinding evaluated;
    evaluated.orderedValues.reserve(plan.orderedExpressions.size());
    for (const auto& expression : plan.orderedExpressions) {
        auto value = evaluateMaterialExpression(expression, context);
        evaluated.values.set(expression.schema.name, value);
        evaluated.orderedValues.push_back({.schema = expression.schema, .value = std::move(value)});
    }
    return evaluated;
}

MaterialStructuredBufferLayout makeMaterialStructuredBufferLayout(const MaterialTemplateSchema& schema) {
    MaterialStructuredBufferLayout layout;
    layout.fields.reserve(schema.fields.size());
    if (schema.fields.empty()) {
        // HLSL has no portable empty struct representation. Keep texture-only
        // MatDesc templates valid with an unobservable one-word value record.
        layout.stride = sizeof(std::uint32_t);
        return layout;
    }
    std::unordered_set<std::string> names;
    std::size_t cursor = 0;
    for (const auto& field : schema.fields) {
        if (field.name.empty() || !names.insert(field.name).second)
            throw std::invalid_argument("FX material structured-buffer schema has an empty or duplicate field name");
        if (field.components == 0 || field.components > 4)
            throw std::invalid_argument("FX material structured-buffer field width is unsupported: " + field.name);
        constexpr std::size_t componentSize = sizeof(std::uint32_t);
        const auto fieldSize = static_cast<std::size_t>(field.components) * componentSize;
        layout.fields.push_back({.schema = field, .offset = cursor, .size = fieldSize});
        if (cursor > std::numeric_limits<std::size_t>::max() - fieldSize)
            throw std::overflow_error("FX material structured-buffer layout size overflow");
        cursor += fieldSize;
    }
    layout.stride = cursor;
    return layout;
}

MaterialStructuredBufferData packMaterialStructuredBuffer(const MaterialStructuredBufferLayout& layout,
                                                          std::span<const EvaluatedMaterialBinding> materials) {
    if (layout.stride == 0 || (layout.fields.empty() && layout.stride != sizeof(std::uint32_t)))
        throw std::invalid_argument("FX material structured-buffer layout is invalid");
    if (materials.size() > std::numeric_limits<std::size_t>::max() / layout.stride)
        throw std::overflow_error("FX material structured-buffer allocation size overflow");

    MaterialStructuredBufferData result{.layout = layout, .bytes = {}, .count = materials.size()};
    result.bytes.resize(materials.size() * layout.stride, std::byte{0});
    for (std::size_t materialIndex = 0; materialIndex < materials.size(); ++materialIndex) {
        const auto& material = materials[materialIndex];
        if (material.orderedValues.size() != layout.fields.size())
            throw std::invalid_argument("evaluated FX material field count does not match its structured layout");
        auto* record = result.bytes.data() + materialIndex * layout.stride;
        for (std::size_t fieldIndex = 0; fieldIndex < layout.fields.size(); ++fieldIndex) {
            const auto& expected = layout.fields[fieldIndex];
            const auto& actual = material.orderedValues[fieldIndex];
            if (actual.schema.name != expected.schema.name || actual.schema.type != expected.schema.type ||
                actual.schema.components != expected.schema.components || expected.offset > layout.stride ||
                expected.size > layout.stride - expected.offset ||
                expected.size != static_cast<std::size_t>(expected.schema.components) * sizeof(std::uint32_t))
                throw std::invalid_argument("evaluated FX material field order/layout mismatch at " +
                                            expected.schema.name);
            storeMaterialField(record + expected.offset, expected.schema, actual.value);
        }
    }
    return result;
}

MaterialGpuTableData makeMaterialGpuTableData(const MaterialTemplateSchema& schema,
                                              std::span<const MaterialGpuTableModel> models) {
    MaterialGpuTableData result;
    std::uint32_t textureSlotCount = 0;
    for (const auto& texture : schema.textures) {
        if (texture.index == std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("FX material texture index cannot be represented as a slot count");
        textureSlotCount = std::max(textureSlotCount, texture.index + 1U);
    }
    result.textureSlotCount = textureSlotCount;

    std::map<std::string, MaterialTextureDesc> textures2D;
    std::map<std::string, MaterialTextureDesc> textures3D;
    std::vector<MaterialGpuTableMaterial> materials;
    result.materialIndices.reserve(models.size());
    std::size_t totalTextureEntries = 0;
    for (const auto& model : models) {
        if (materials.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("FX material row index exceeds 32-bit table indices");
        result.materialIndices.push_back(static_cast<std::uint32_t>(materials.size()));
        if (model.materials.size() > std::numeric_limits<std::size_t>::max() - materials.size())
            throw std::overflow_error("FX material row count overflow");
        materials.insert(materials.end(), model.materials.begin(), model.materials.end());
        if (textureSlotCount != 0 &&
            model.materials.size() > (std::numeric_limits<std::size_t>::max() - totalTextureEntries) /
                                         static_cast<std::size_t>(textureSlotCount))
            throw std::overflow_error("FX material texture table size overflow");
        totalTextureEntries += model.materials.size() * static_cast<std::size_t>(textureSlotCount);
    }
    if (materials.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("FX material row count exceeds 32-bit table indices");

    const auto rememberTexture = [&](const MaterialGpuTableMaterial& material,
                                     const MaterialTextureSchema& textureSchema) {
        if (material.binding == nullptr || material.evaluated == nullptr)
            throw std::invalid_argument("FX material table contains a null binding or evaluated value");
        const auto& plan = *material.binding;
        if (plan.orderedTextures.size() != schema.textures.size())
            throw std::invalid_argument("FX material texture plan does not match its ordered schema");
        const auto field = std::find_if(
            plan.orderedTextures.begin(), plan.orderedTextures.end(),
            [&textureSchema](const auto& candidate) { return candidate.schema.name == textureSchema.name; });
        if (field == plan.orderedTextures.end() || field->schema.index != textureSchema.index ||
            field->schema.dimension != textureSchema.dimension || field->schema.mipmapped != textureSchema.mipmapped)
            throw std::invalid_argument("FX material texture plan has a mismatched schema field: " +
                                        textureSchema.name);
        if (!field->physicalTextureIndex.has_value())
            return;
        if (*field->physicalTextureIndex >= plan.layout.uniqueTextures.size())
            throw std::invalid_argument("FX material texture plan has an invalid physical texture index: " +
                                        textureSchema.name);
        const auto& descriptor = plan.layout.uniqueTextures[*field->physicalTextureIndex];
        if (descriptor.dimension != textureSchema.dimension || descriptor.mipmapped != textureSchema.mipmapped)
            throw std::invalid_argument("FX material physical texture does not match its schema: " +
                                        textureSchema.name);
        auto& catalog = textureSchema.dimension == MaterialTextureDimension::twoD ? textures2D : textures3D;
        catalog.try_emplace(textureKeyString(makeTextureKey(descriptor)), descriptor);
    };
    for (const auto& material : materials)
        for (const auto& texture : schema.textures)
            rememberTexture(material, texture);

    if (textures2D.size() > std::numeric_limits<std::uint32_t>::max() ||
        textures3D.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("FX material physical texture count exceeds 32-bit descriptor indices");
    std::map<std::string, std::uint32_t> indices2D;
    std::map<std::string, std::uint32_t> indices3D;
    const auto materializeCatalog = [](const auto& source, auto& destination, auto& indices) {
        destination.reserve(source.size());
        std::uint32_t index = 0;
        for (const auto& [key, descriptor] : source) {
            indices.emplace(key, index++);
            destination.push_back(descriptor);
        }
    };
    materializeCatalog(textures2D, result.textures2D, indices2D);
    materializeCatalog(textures3D, result.textures3D, indices3D);

    result.textureIndices2D.assign(totalTextureEntries, kMissingMaterialTextureIndex);
    result.textureIndices3D.assign(totalTextureEntries, kMissingMaterialTextureIndex);
    std::vector<EvaluatedMaterialBinding> evaluatedValues;
    evaluatedValues.reserve(materials.size());
    const auto layout = makeMaterialStructuredBufferLayout(schema);
    std::size_t materialIndex = 0;
    for (const auto& model : models) {
        for (const auto& material : model.materials) {
            if (material.binding == nullptr || material.evaluated == nullptr)
                throw std::invalid_argument("FX material table contains a null binding or evaluated value");
            if (material.binding->orderedTextures.size() != schema.textures.size())
                throw std::invalid_argument("FX material texture plan does not match its ordered schema");
            const auto base = materialIndex * static_cast<std::size_t>(textureSlotCount);
            for (const auto& textureSchema : schema.textures) {
                const auto field = std::find_if(
                    material.binding->orderedTextures.begin(), material.binding->orderedTextures.end(),
                    [&textureSchema](const auto& candidate) { return candidate.schema.name == textureSchema.name; });
                if (field == material.binding->orderedTextures.end() || field->schema.index != textureSchema.index ||
                    field->schema.dimension != textureSchema.dimension ||
                    field->schema.mipmapped != textureSchema.mipmapped)
                    throw std::invalid_argument("FX material texture plan has a mismatched schema field: " +
                                                textureSchema.name);
                if (!field->physicalTextureIndex.has_value())
                    continue;
                const auto& descriptor = material.binding->layout.uniqueTextures[*field->physicalTextureIndex];
                const auto key = textureKeyString(makeTextureKey(descriptor));
                const auto& indices = textureSchema.dimension == MaterialTextureDimension::twoD ? indices2D : indices3D;
                const auto physical = indices.find(key);
                if (physical == indices.end())
                    throw std::logic_error("FX material physical texture is absent from its table catalog");
                auto& destinations = textureSchema.dimension == MaterialTextureDimension::twoD
                                         ? result.textureIndices2D
                                         : result.textureIndices3D;
                destinations[base + textureSchema.index] = physical->second;
            }
            evaluatedValues.push_back(*material.evaluated);
            ++materialIndex;
        }
    }
    result.values = packMaterialStructuredBuffer(layout, evaluatedValues);
    return result;
}

MaterialGpuTableData makeMaterialGpuTableData(const MaterialTemplateSchema& schema,
                                              std::span<const MaterialGpuTableInputModel> models) {
    std::size_t totalMaterials = 0;
    for (const auto& model : models) {
        if (model.materials.size() > std::numeric_limits<std::size_t>::max() - totalMaterials)
            throw std::overflow_error("FX material input row count overflow");
        totalMaterials += model.materials.size();
    }
    if (totalMaterials > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("FX material input row count exceeds 32-bit table indices");

    std::vector<MaterialBindingPlan> plans;
    std::vector<EvaluatedMaterialBinding> evaluated;
    std::vector<MaterialGpuTableMaterial> linkedMaterials;
    plans.reserve(totalMaterials);
    evaluated.reserve(totalMaterials);
    linkedMaterials.reserve(totalMaterials);
    std::vector<MaterialGpuTableModel> linkedModels;
    linkedModels.reserve(models.size());

    for (const auto& model : models) {
        const auto start = linkedMaterials.size();
        for (const auto& input : model.materials) {
            plans.push_back(linkMaterial(schema, input.instance));
            evaluated.push_back(evaluateMaterialValues(plans.back(), input.context));
            linkedMaterials.push_back({.binding = &plans.back(), .evaluated = &evaluated.back()});
        }
        linkedModels.push_back(
            {.materials =
                 std::span<const MaterialGpuTableMaterial>(linkedMaterials).subspan(start, model.materials.size())});
    }
    return makeMaterialGpuTableData(schema, linkedModels);
}

} // namespace dayo::core::fx
