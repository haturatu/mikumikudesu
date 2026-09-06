#include "fx/fx_shader_cache.hpp"

#include <functional>

namespace dayo::fx {

FxShaderCache::Handle FxShaderCache::getOrCompile(const FxShaderKey& key, const std::string& source) {
    const auto combined = key.combined() + "|" + std::to_string(std::hash<std::string>{}(source));
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(combined);
    if (found != entries_.end())
        return found->second;
    const Handle handle = next_++;
    entries_.emplace(combined, handle);
    return handle;
}

std::optional<FxShaderCache::Handle> FxShaderCache::find(const FxShaderKey& key) const {
    std::scoped_lock lock(mutex_);
    for (const auto& [stored, handle] : entries_) {
        if (stored.rfind(key.combined(), 0) == 0)
            return handle;
    }
    return std::nullopt;
}

void FxShaderCache::clear() noexcept {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

std::size_t FxShaderCache::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

} // namespace dayo::fx
