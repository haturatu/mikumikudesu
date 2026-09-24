#pragma once

#include "fx/fx_compiler.hpp"
#include "graphics/fx_resource_runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::graphics {

// Non-owning snapshots of resources declared with shared=source. The FX
// runtime remains the physical owner; consumers must retain the source and
// generation in their cache identity and relink when either changes.
class FxSharedResourceRegistry {
  public:
    struct Export {
        std::string owner;
        FxResourceStore::Resource resource;
        std::uint64_t generation{};
    };

    enum class LookupStatus : std::uint8_t { notFound, unique, ambiguous };

    struct LookupResult {
        LookupStatus status{LookupStatus::notFound};
        std::optional<Export> value;
    };

    // Replaces the snapshot for one runtime owner. Only physical resources
    // named by a shared=source declaration are published. Re-publishing an
    // unchanged snapshot preserves its generation.
    void publish(std::string owner, const fx::FxProgram& program, const FxResourceStore& store);
    void invalidateOwner(std::string_view owner) noexcept;
    void clear() noexcept;

    [[nodiscard]] LookupResult resolve(std::string_view name) const;
    [[nodiscard]] std::span<const Export> exports() const noexcept {
        return exports_;
    }

  private:
    std::vector<Export> exports_;
    std::uint64_t nextGeneration_{1};

    [[nodiscard]] std::uint64_t allocateGeneration();
};

} // namespace dayo::graphics
