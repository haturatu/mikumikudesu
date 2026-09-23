#include "graphics/deformer_resource_registry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace dayo::graphics {
namespace {

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

std::uint64_t DeformerResourceRegistry::allocateGeneration() {
    if (nextGeneration_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("deformer resource generation exhausted");
    return nextGeneration_++;
}

void DeformerResourceRegistry::publish(core::ModelId owner, core::EffectId effect, const FxResourceStore& store) {
    std::vector<const FxResourceStore::Resource*> validResources;
    validResources.reserve(store.size());
    for (const auto& resource : store.resources()) {
        if (valid(resource))
            validResources.push_back(&resource);
    }

    std::vector<const Export*> previous;
    for (const auto& entry : exports_) {
        if (entry.owner == owner && entry.effect == effect)
            previous.push_back(&entry);
    }
    std::ranges::sort(validResources, {}, [](const auto* resource) -> std::string_view { return resource->name; });
    std::ranges::sort(previous, {}, [](const auto* entry) -> std::string_view { return entry->resource.name; });

    const bool unchanged = validResources.size() == previous.size() &&
                           std::ranges::equal(validResources, previous, [](const auto* resource, const auto* entry) {
                               return sameResource(*resource, entry->resource);
                           });
    if (validResources.empty()) {
        invalidateEffect(effect);
        return;
    }
    const auto generation = unchanged ? previous.front()->generation : allocateGeneration();

    std::erase_if(exports_,
                  [owner, effect](const Export& entry) { return entry.owner == owner && entry.effect == effect; });
    exports_.reserve(exports_.size() + validResources.size());
    for (const auto* resource : validResources)
        exports_.push_back({.owner = owner, .effect = effect, .resource = *resource, .generation = generation});
}

void DeformerResourceRegistry::invalidateEffect(core::EffectId effect) noexcept {
    std::erase_if(exports_, [effect](const Export& entry) { return entry.effect == effect; });
}

void DeformerResourceRegistry::clear() noexcept {
    exports_.clear();
}

DeformerResourceRegistry::LookupResult DeformerResourceRegistry::resolve(core::ModelId owner,
                                                                         std::string_view name) const {
    LookupResult result;
    for (const auto& entry : exports_) {
        if (entry.owner != owner || entry.resource.name != name)
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
