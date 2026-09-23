#pragma once

#include "core/scene.hpp"
#include "graphics/fx_resource_runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dayo::graphics {

// Non-owning exports of resources created by a model's deform FX. The FX
// runtime remains the resource owner; consumers must treat generation as part
// of the handle and relink when it changes.
class DeformerResourceRegistry {
  public:
    struct Export {
        core::ModelId owner{};
        core::EffectId effect{};
        FxResourceStore::Resource resource;
        std::uint64_t generation{};
    };

    enum class LookupStatus : std::uint8_t { notFound, unique, ambiguous };

    struct LookupResult {
        LookupStatus status{LookupStatus::notFound};
        std::optional<Export> value;
    };

    // Replaces the snapshot for this owner/effect. Re-publishing an unchanged
    // snapshot preserves its generation; a changed resource set gets a new
    // generation so dependent descriptors can be rebuilt.
    void publish(core::ModelId owner, core::EffectId effect, const FxResourceStore& store);
    void invalidateEffect(core::EffectId effect) noexcept;
    void clear() noexcept;

    [[nodiscard]] LookupResult resolve(core::ModelId owner, std::string_view name) const;
    [[nodiscard]] std::span<const Export> exports() const noexcept {
        return exports_;
    }

  private:
    std::vector<Export> exports_;
    std::uint64_t nextGeneration_{1};

    [[nodiscard]] std::uint64_t allocateGeneration();
};

} // namespace dayo::graphics
