#include "core/fx/fx_material.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
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
        if (std::find(stack.begin(), stack.end(), key) != stack.end() ||
            std::find(stack.begin(), stack.end(), ref) != stack.end()) {
            // Alias cycle: deterministic fallback to the smallest id in the cycle.
            std::string smallest = key;
            for (const auto& entry : stack)
                smallest = std::min(smallest, entry);
            smallest = std::min(smallest, ref);
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

bool MaterialGpuLayout::hasLocal(std::string_view localId) const noexcept {
    try {
        return localToCanonical.contains(std::string(localId)) ||
               localToCanonical.contains(trimCopy(localId));
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
