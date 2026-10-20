#pragma once

#include "core/scene.hpp"

#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace dayo::app {

// The combined texture array follows scene order, including models without
// geometry. Revalidate counts once per frame; each model lookup is constant time.
class PreviewTextureLayout {
  public:
    bool update(std::span<const core::ModelInstance> models) {
        bool changed = entries_.size() != models.size();
        if (!changed)
            for (std::size_t i = 0; i < models.size(); ++i)
                changed = changed || entries_[i].id != models[i].id || entries_[i].count != models[i].textures.size();
        if (!changed)
            return false;
        std::uint64_t base = 0;
        entries_.resize(models.size());
        for (std::size_t i = 0; i < models.size(); ++i) {
            base += models[i].textures.size();
            // Slot zero is reserved for the untextured fallback.
            if (base >= std::numeric_limits<std::uint32_t>::max())
                throw std::overflow_error("combined preview texture slots exceed uint32");
            entries_[i] = {models[i].id, models[i].textures.size(),
                           static_cast<std::uint32_t>(base - models[i].textures.size())};
        }
        ++generation_;
        return true;
    }
    [[nodiscard]] std::uint32_t base(std::size_t index) const {
        return entries_.at(index).base;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

  private:
    struct Entry {
        core::ModelId id{};
        std::size_t count{};
        std::uint32_t base{};
    };
    std::vector<Entry> entries_;
    std::uint64_t generation_{};
};

} // namespace dayo::app
