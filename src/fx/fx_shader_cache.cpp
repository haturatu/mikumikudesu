#include "fx/fx_shader_cache.hpp"

#include <algorithm>
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

FxShaderArtifact FxShaderCache::compileOrGet(const FxShaderKey& key, const FxShaderCompileRequest& request,
                                             const FxShaderCompiler& compiler) {
    const auto id = key.combined();
    {
        std::scoped_lock lock(mutex_);
        const auto found = entries_.find(id);
        if (found != entries_.end()) {
            const auto artifact = artifacts_.find(found->second);
            if (artifact != artifacts_.end())
                return artifact->second;
        }
    }

    FxShaderArtifact artifact = compiler.compile(request);
    std::scoped_lock lock(mutex_);
    const auto existing = entries_.find(id);
    if (existing != entries_.end()) {
        const auto cached = artifacts_.find(existing->second);
        if (cached != artifacts_.end())
            return cached->second;
    }
    const Handle handle = next_++;
    entries_.emplace(id, handle);
    artifacts_.emplace(handle, artifact);
    return artifact;
}

std::optional<FxShaderCache::Handle> FxShaderCache::findExact(const FxShaderKey& key, const std::string& source) const {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(key.combined() + "|" + std::to_string(std::hash<std::string>{}(source)));
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

std::optional<std::vector<std::uint32_t>> FxShaderCache::binary(Handle handle) const {
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find_if(artifacts_, [handle](const auto& entry) { return entry.first == handle; });
    if (found == artifacts_.end())
        return std::nullopt;
    return found->second.spirv;
}

void FxShaderCache::clear() noexcept {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    artifacts_.clear();
}

std::size_t FxShaderCache::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

FxPipelineCache::Handle FxPipelineCache::getOrCreate(const FxPipelineKey& key) {
    std::scoped_lock lock(mutex_);
    const auto id = key.combined();
    const auto found = entries_.find(id);
    if (found != entries_.end())
        return found->second;
    const Handle handle = next_++;
    entries_.emplace(id, handle);
    return handle;
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
