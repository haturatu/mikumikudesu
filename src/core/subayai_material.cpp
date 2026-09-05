#include "core/subayai_material.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace dayo::core {
namespace {

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string removeComment(std::string_view line) {
    return trim(line.substr(0, line.find('#')));
}

std::pair<std::string, std::string> assignment(std::string_view line, const std::filesystem::path& path,
                                               std::size_t lineNumber) {
    const auto separator = line.find(':');
    if (separator == std::string_view::npos || trim(line.substr(0, separator)).empty())
        throw std::runtime_error("invalid Subayai annotation at " + path.string() + ':' + std::to_string(lineNumber));
    return {trim(line.substr(0, separator)), trim(line.substr(separator + 1))};
}

std::vector<std::string> splitValues(std::string_view value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto separator = value.find(',', start);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        result.push_back(trim(value.substr(start, end - start)));
        if (separator == std::string_view::npos)
            break;
        start = separator + 1;
    }
    return result;
}

template <typename T> T integer(std::string_view value, const std::filesystem::path& path, std::size_t lineNumber) {
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid integer in Subayai annotation at " + path.string() + ':' +
                                 std::to_string(lineNumber) + ": " + std::string(value));
    return result;
}

float floating(std::string_view value, const std::filesystem::path& path, std::size_t lineNumber) {
    std::istringstream input{std::string(value)};
    float result{};
    char extra{};
    if (!(input >> result) || (input >> extra))
        throw std::runtime_error("invalid floating-point value in Subayai annotation at " + path.string() + ':' +
                                 std::to_string(lineNumber) + ": " + std::string(value));
    return result;
}

std::string textureName(std::string name) {
    if (name.starts_with("_T"))
        name.erase(0, 2);
    return trim(name);
}

SubayaiParameterDefinition definitionFor(const SubayaiMaterialSchema* schema, std::string_view name,
                                         std::size_t componentCount, const std::filesystem::path& path,
                                         std::size_t lineNumber) {
    if (schema != nullptr) {
        const auto found = schema->parameters.find(std::string(name));
        if (found == schema->parameters.end())
            throw std::runtime_error("unknown Subayai material parameter at " + path.string() + ':' +
                                     std::to_string(lineNumber) + ": " + std::string(name));
        return found->second;
    }
    return {SubayaiParameterType::floating,
            static_cast<std::uint8_t>(std::clamp(componentCount, std::size_t{1}, std::size_t{4}))};
}

MaterialValue parseValue(std::string_view name, std::string_view text, const SubayaiMaterialSchema* schema,
                         const std::filesystem::path& path, std::size_t lineNumber) {
    const auto values = splitValues(text);
    if (values.empty() || std::ranges::any_of(values, [](const auto& value) { return value.empty(); }))
        throw std::runtime_error("empty Subayai material value at " + path.string() + ':' + std::to_string(lineNumber));

    const auto definition = definitionFor(schema, name, values.size(), path, lineNumber);
    if (definition.type == SubayaiParameterType::texture)
        return std::filesystem::path(std::string(text));

    if (definition.type == SubayaiParameterType::integer) {
        std::array<std::int32_t, 4> parsed{};
        for (std::size_t index = 0; index < values.size(); ++index)
            parsed[index] = integer<std::int32_t>(values[index], path, lineNumber);
        switch (values.size()) {
        case 1:
            return parsed[0];
        case 2:
            return std::array<std::int32_t, 2>{parsed[0], parsed[1]};
        case 3:
            return std::array<std::int32_t, 3>{parsed[0], parsed[1], parsed[2]};
        case 4:
            return parsed;
        default:
            break;
        }
    } else {
        std::array<float, 4> parsed{};
        try {
            for (std::size_t index = 0; index < values.size(); ++index)
                parsed[index] = floating(values[index], path, lineNumber);
        } catch (const std::runtime_error&) {
            // Subayai permits animated expressions such as
            // `5*(sin(Time*2*pi)+1)` in scalar fields. Keep those expressions
            // intact for a future executor instead of rejecting the preset.
            return std::string(text);
        }
        switch (values.size()) {
        case 1:
            return parsed[0];
        case 2:
            return std::array<float, 2>{parsed[0], parsed[1]};
        case 3:
            return std::array<float, 3>{parsed[0], parsed[1], parsed[2]};
        case 4:
            return parsed;
        default:
            break;
        }
    }
    throw std::runtime_error("Subayai material values have more than four components at " + path.string() + ':' +
                             std::to_string(lineNumber));
}

std::string resolveEnum(const SubayaiMaterialSchema& schema, std::string_view name, std::string value) {
    const auto found = schema.enumerations.find(std::string(name));
    if (found == schema.enumerations.end())
        return value;
    const auto alias = found->second.find(value);
    return alias == found->second.end() ? value : std::to_string(alias->second);
}

void parseEnum(SubayaiMaterialSchema& schema, std::string_view line, const std::filesystem::path& path,
               std::size_t lineNumber) {
    const auto [name, valuesText] = assignment(line.substr(2), path, lineNumber);
    auto& values = schema.enumerations[name];
    std::int32_t next = 0;
    for (const auto& item : splitValues(valuesText)) {
        const auto separator = item.find('=');
        const auto label = trim(item.substr(0, separator));
        if (label.empty())
            throw std::runtime_error("invalid Subayai enum at " + path.string() + ':' + std::to_string(lineNumber));
        const auto value = separator == std::string::npos
                               ? next
                               : integer<std::int32_t>(trim(item.substr(separator + 1)), path, lineNumber);
        values[label] = value;
        next = value + 1;
    }
}

SubayaiMaterialSchema readSchema(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open Subayai material schema: " + path.string());

    SubayaiMaterialSchema schema;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto clean = removeComment(line);
        if (clean.empty())
            continue;
        if (clean.starts_with("_E")) {
            parseEnum(schema, clean, path, lineNumber);
            continue;
        }
        if (clean.starts_with("_R"))
            continue;
        if (clean.starts_with("_T")) {
            const auto [unused, rawName] = assignment(clean, path, lineNumber);
            static_cast<void>(unused);
            schema.parameters[trim(rawName)] = {SubayaiParameterType::texture, 1};
            continue;
        }
        if (clean.starts_with("f.") || clean.starts_with("i.")) {
            const auto separator = clean.find(':');
            if (separator == std::string::npos)
                throw std::runtime_error("invalid Subayai parameter declaration at " + path.string() + ':' +
                                         std::to_string(lineNumber));
            const auto header = trim(clean.substr(0, separator));
            const auto componentCount = integer<std::uint32_t>(header.substr(2), path, lineNumber);
            if (componentCount == 0 || componentCount > 4)
                throw std::runtime_error("Subayai parameter component count is outside 1..4 at " + path.string() + ':' +
                                         std::to_string(lineNumber));
            schema.parameters[trim(clean.substr(separator + 1))] = {clean[0] == 'i' ? SubayaiParameterType::integer
                                                                                    : SubayaiParameterType::floating,
                                                                    static_cast<std::uint8_t>(componentCount)};
            continue;
        }
        const auto [name, value] = assignment(clean, path, lineNumber);
        if (schema.parameters.contains(name))
            schema.defaults.set(name, parseValue(name, resolveEnum(schema, name, value), &schema, path, lineNumber));
    }
    return schema;
}

SubayaiMaterialPreset readPreset(const std::filesystem::path& path, const SubayaiMaterialSchema* schema) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open Subayai material preset: " + path.string());

    SubayaiMaterialPreset preset{.sourcePath = path, .parameters = {}};
    if (schema != nullptr)
        preset.parameters = schema->defaults;

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto clean = removeComment(line);
        if (clean.empty())
            continue;
        const auto [rawName, rawValue] = assignment(clean, path, lineNumber);
        const bool texture = rawName.starts_with("_T");
        const auto name = texture ? textureName(rawName) : rawName;
        auto value = rawValue;
        if (schema != nullptr && !texture)
            value = resolveEnum(*schema, name, value);
        const auto parsed = texture && schema == nullptr ? MaterialValue{std::filesystem::path(value)}
                                                         : parseValue(name, value, schema, path, lineNumber);
        if (texture && std::holds_alternative<std::filesystem::path>(parsed)) {
            auto texturePath = std::get<std::filesystem::path>(parsed);
            if (texturePath.is_relative())
                texturePath = path.parent_path() / texturePath;
            preset.parameters.set(name, std::move(texturePath));
        } else {
            preset.parameters.set(name, parsed);
        }
    }
    return preset;
}

} // namespace

SubayaiMaterialSchema loadSubayaiMaterialSchema(const std::filesystem::path& path) {
    return readSchema(path);
}

SubayaiMaterialPreset loadSubayaiMaterialPreset(const std::filesystem::path& path,
                                                const SubayaiMaterialSchema& schema) {
    return readPreset(path, &schema);
}

SubayaiMaterialPreset loadSubayaiMaterialPreset(const std::filesystem::path& path) {
    return readPreset(path, nullptr);
}

} // namespace dayo::core
