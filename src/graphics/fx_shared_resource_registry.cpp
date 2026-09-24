#include "graphics/fx_shared_resource_registry.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace dayo::graphics {
namespace {

[[nodiscard]] bool isSharedSource(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        value.remove_suffix(1);
    if (value.size() != 6)
        return false;
    constexpr std::string_view source = "source";
    return std::ranges::equal(value, source, [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    });
}

[[nodiscard]] bool valid(const FxResourceStore::Resource& resource) noexcept {
    switch (resource.kind) {
    case FxResourceStore::Kind::texture:
        return resource.texture.valid();
    case FxResourceStore::Kind::buffer:
        return resource.buffer.valid();
    case FxResourceStore::Kind::sampler:
        return resource.sampler.valid();
    }
    return false;
}

[[nodiscard]] bool sameResource(const FxResourceStore::Resource& left,
                                const FxResourceStore::Resource& right) noexcept {
    return left.name == right.name && left.kind == right.kind && left.texture == right.texture &&
           left.buffer == right.buffer && left.sampler == right.sampler && left.extent.width == right.extent.width &&
           left.extent.height == right.extent.height && left.extent.depth == right.extent.depth &&
           left.format == right.format && left.dimension == right.dimension &&
           left.legacyDescriptorKind == right.legacyDescriptorKind && left.legacyBinding == right.legacyBinding;
}

} // namespace

std::uint64_t FxSharedResourceRegistry::allocateGeneration() {
    if (nextGeneration_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("shared FX resource generation exhausted");
    return nextGeneration_++;
}

void FxSharedResourceRegistry::publish(std::string owner, const fx::FxProgram& program, const FxResourceStore& store) {
    if (owner.empty())
        throw std::invalid_argument("shared FX resource owner must not be empty");

    std::vector<const FxResourceStore::Resource*> candidates;
    const auto append = [&](std::span<const core::EffectTexture> declarations, std::uint32_t dimension) {
        for (const auto& declaration : declarations) {
            if (!isSharedSource(declaration.shared))
                continue;
            const auto* resource = store.find(declaration.name);
            if (resource == nullptr || resource->kind != FxResourceStore::Kind::texture ||
                resource->dimension != dimension || !valid(*resource))
                continue;
            candidates.push_back(resource);
        }
    };
    append(program.textures, 2);
    append(program.textures3D, 3);
    for (const auto& declaration : program.buffers) {
        if (!isSharedSource(declaration.shared))
            continue;
        const auto* resource = store.find(declaration.name);
        if (resource != nullptr && resource->kind == FxResourceStore::Kind::buffer && valid(*resource))
            candidates.push_back(resource);
    }

    std::ranges::sort(candidates, {}, [](const auto* resource) { return resource->name; });
    std::vector<const Export*> previous;
    for (const auto& entry : exports_) {
        if (entry.owner == owner)
            previous.push_back(&entry);
    }
    std::ranges::sort(previous, {}, [](const auto* entry) { return entry->resource.name; });
    const bool unchanged = candidates.size() == previous.size() &&
                           std::ranges::equal(candidates, previous, [](const auto* resource, const auto* entry) {
                               return sameResource(*resource, entry->resource);
                           });
    if (candidates.empty()) {
        invalidateOwner(owner);
        return;
    }
    const auto generation = unchanged ? previous.front()->generation : allocateGeneration();
    invalidateOwner(owner);
    exports_.reserve(exports_.size() + candidates.size());
    for (const auto* resource : candidates)
        exports_.push_back({.owner = owner, .resource = *resource, .generation = generation});
}

void FxSharedResourceRegistry::invalidateOwner(std::string_view owner) noexcept {
    std::erase_if(exports_, [owner](const Export& entry) { return entry.owner == owner; });
}

void FxSharedResourceRegistry::clear() noexcept {
    exports_.clear();
}

FxSharedResourceRegistry::LookupResult FxSharedResourceRegistry::resolve(std::string_view name) const {
    LookupResult result;
    for (const auto& entry : exports_) {
        if (entry.resource.name != name)
            continue;
        if (result.value.has_value()) {
            result.status = LookupStatus::ambiguous;
            result.value.reset();
            return result;
        }
        result.status = LookupStatus::unique;
        result.value = entry;
    }
    return result;
}

} // namespace dayo::graphics
