#include "fx/fx_shader_cache.hpp"

#include <functional>

namespace dayo::fx {

namespace {

[[nodiscard]] std::string fullKey(const FxShaderKey& key, const std::string& source) {
    return key.combined() + "|src=" + std::to_string(std::hash<std::string>{}(source));
}

} // namespace

FxShaderCache::Handle FxShaderCache::getOrCompile(const FxShaderKey& key, const std::string& source) {
    const auto full = fullKey(key, source);
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(full);
    if (found != entries_.end())
        return found->second;
    const Handle handle = next_++;
    entries_.emplace(full, handle);
    return handle;
}

std::optional<FxShaderCache::Handle> FxShaderCache::findExact(const FxShaderKey& key, const std::string& source) const {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(fullKey(key, source));
    if (found == entries_.end())
        return std::nullopt;
    return found->second;
}

std::optional<FxShaderCache::Handle> FxShaderCache::find(const FxShaderKey& key) const {
    std::scoped_lock lock(mutex_);
    for (const auto& [stored, handle] : entries_) {
        if (stored.starts_with(key.combined()))
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

FxPipelineCache::Handle FxPipelineCache::getOrCreate(const FxPipelineKey& key) {
    const auto full = key.combined();
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(full);
    if (found != entries_.end())
        return found->second;
    const Handle handle = next_++;
    entries_.emplace(full, handle);
    return handle;
}

std::optional<FxPipelineCache::Handle> FxPipelineCache::find(const FxPipelineKey& key) const {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(key.combined());
    if (found == entries_.end())
        return std::nullopt;
    return found->second;
}

void FxPipelineCache::clear() noexcept {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

std::size_t FxPipelineCache::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

} // namespace dayo::fx
