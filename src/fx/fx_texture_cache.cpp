#include "fx/fx_texture_cache.hpp"

namespace dayo::fx {

FxTextureCache::Handle FxTextureCache::store(const FxTextureKey& key, std::uint32_t width, std::uint32_t height,
                                             bool hasTransparency) {
    std::scoped_lock lock(mutex_);
    const auto id = key.combined();
    const auto found = entries_.find(id);
    if (found != entries_.end()) {
        found->second.width = width;
        found->second.height = height;
        found->second.hasTransparency = hasTransparency;
        return found->second.handle;
    }
    const Handle handle = next_++;
    entries_.emplace(id, FxTextureEntry{handle, width, height, hasTransparency});
    return handle;
}

std::optional<FxTextureEntry> FxTextureCache::find(const FxTextureKey& key) const {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(key.combined());
    if (found == entries_.end())
        return std::nullopt;
    return found->second;
}

void FxTextureCache::clear() noexcept {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

std::size_t FxTextureCache::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

} // namespace dayo::fx
