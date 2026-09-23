#include "core/fx/fx_material.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
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
    while (start <= text.size()) {
        const auto end = text.find(',', start);
        const auto length = end == std::string_view::npos ? text.size() - start : end - start;
        const auto value = trimCopy(text.substr(start, length));
        if (value.empty())
            throw std::invalid_argument("empty item in upstream material value list");
        values.push_back(value);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return values;
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
    std::int32_t nextValue = 0;
    const auto items = splitMaterialList(valueList);
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
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
                                    Parser&& parser) {
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

void overlayMaterialDefaults(MaterialTemplateSchema& schema, std::string_view source) {
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        const auto lineEnd = source.find('\n', lineStart);
        const auto length = lineEnd == std::string_view::npos ? source.size() - lineStart : lineEnd - lineStart;
        auto line = source.substr(lineStart, length);
        if (lineStart == 0 && line.starts_with("\xEF\xBB\xBF"))
            line.remove_prefix(3);
        if (const auto comment = line.find('#'); comment != std::string_view::npos)
            line = line.substr(0, comment);
        const auto separator = line.find(':');
        if (separator != std::string_view::npos) {
            const auto fieldName = materialIdentifier(trimCopy(line.substr(0, separator)));
            const auto* field = findMaterialField(schema, fieldName);
            const auto values = trimCopy(line.substr(separator + 1));
            if (field != nullptr && !values.empty())
                setMaterialValue(schema, *field, splitMaterialList(values));
        }
        if (lineEnd == std::string_view::npos)
            break;
        lineStart = lineEnd + 1;
    }
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
        if (const auto comment = line.find('#'); comment != std::string_view::npos)
            line = line.substr(0, comment);
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
                } else if (left.starts_with("_T") || left.starts_with("_V")) {
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
    overlayMaterialDefaults(result, source);
    return result;
}

MaterialTemplateSchema loadMaterialTemplateSchema(const std::filesystem::path& path, std::string name) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open upstream material template: " + path.string());
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad())
        throw std::runtime_error("cannot read upstream material template: " + path.string());
    return parseMaterialTemplateSchema(source, std::move(name));
}

void applyMaterialDefaultFile(MaterialTemplateSchema& schema, std::string_view source) {
    schema.defaultFileSourceText = source;
    overlayMaterialDefaults(schema, source);
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
    applyMaterialDefaultFile(schema, source);
}

std::size_t MaterialTextureKeyHash::operator()(const MaterialTextureKey& key) const noexcept {
    std::size_t seed = std::hash<std::string>{}(key.path);
    seed ^= std::hash<std::string>{}(key.format) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<std::string>{}(key.colorspace) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<std::string>{}(key.mipPolicy) + 0x9E3779B9U + (seed << 6U) + (seed >> 2U);
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
        .format = normalizeTextureToken(desc.format),
        .colorspace = normalizeTextureToken(desc.colorspace),
        .mipPolicy = normalizeTextureToken(desc.mipPolicy),
    };
}

std::string textureKeyString(const MaterialTextureKey& key) {
    return key.path + "|" + key.format + "|" + key.colorspace + "|" + key.mipPolicy;
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
            .format = normalizeTextureToken(decl.texture.format),
            .colorspace = normalizeTextureToken(decl.texture.colorspace),
            .mipPolicy = normalizeTextureToken(decl.texture.mipPolicy),
        };
        const MaterialTextureKey key{
            .path = normalized.path,
            .format = normalized.format,
            .colorspace = normalized.colorspace,
            .mipPolicy = normalized.mipPolicy,
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
    MaterialBindingPlan plan{.layout = linkMaterialLayout(templ, instance), .resolvedParameters = {}};
    plan.resolvedParameters = templ.defaults;
    if (instance != nullptr) {
        for (const auto& [name, value] : instance->overrides.values())
            plan.resolvedParameters.set(name, value);
    }
    log::debug("fx material '", templ.name, "' binding plan with ", plan.layout.canonicalToSlot.size(), " slots");
    return plan;
}

} // namespace dayo::core::fx
