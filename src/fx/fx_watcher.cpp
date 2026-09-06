#include "fx/fx_watcher.hpp"

#include <algorithm>

namespace dayo::fx {

std::string FxAssetWatcher::key(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

void FxAssetWatcher::addEffect(std::string effect, std::vector<std::filesystem::path> files) {
    std::scoped_lock lock(mutex_);
    std::unordered_set<std::string> normalized;
    for (auto& file : files) {
        const auto id = key(file);
        normalized.insert(id);
        reverseDeps_[id].insert(effect);
    }
    // Drop stale reverse edges for re-registered effects.
    const auto previous = forwardDeps_.find(effect);
    if (previous != forwardDeps_.end()) {
        for (const auto& old : previous->second) {
            if (!normalized.contains(old)) {
                const auto reverse = reverseDeps_.find(old);
                if (reverse != reverseDeps_.end()) {
                    reverse->second.erase(effect);
                    if (reverse->second.empty())
                        reverseDeps_.erase(reverse);
                }
            }
        }
    }
    forwardDeps_[effect] = std::move(normalized);
}

void FxAssetWatcher::removeEffect(const std::string& effect) {
    std::scoped_lock lock(mutex_);
    const auto forward = forwardDeps_.find(effect);
    if (forward != forwardDeps_.end()) {
        for (const auto& file : forward->second) {
            const auto reverse = reverseDeps_.find(file);
            if (reverse != reverseDeps_.end()) {
                reverse->second.erase(effect);
                if (reverse->second.empty())
                    reverseDeps_.erase(reverse);
            }
        }
        forwardDeps_.erase(forward);
    }
    dirty_.erase(effect);
}

std::vector<std::string> FxAssetWatcher::notifyChanged(const std::filesystem::path& file) {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> affected;
    const auto found = reverseDeps_.find(key(file));
    if (found != reverseDeps_.end()) {
        affected.assign(found->second.begin(), found->second.end());
        std::sort(affected.begin(), affected.end());
        for (const auto& effect : affected)
            dirty_.insert(effect);
    }
    return affected;
}

std::vector<std::string> FxAssetWatcher::takeDirty() {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> result(dirty_.begin(), dirty_.end());
    std::sort(result.begin(), result.end());
    dirty_.clear();
    return result;
}

bool FxAssetWatcher::hasDirty(const std::string& effect) const {
    std::scoped_lock lock(mutex_);
    return dirty_.contains(effect);
}

void FxAssetWatcher::clear() noexcept {
    std::scoped_lock lock(mutex_);
    reverseDeps_.clear();
    forwardDeps_.clear();
    dirty_.clear();
}

std::size_t FxAssetWatcher::effectCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return forwardDeps_.size();
}

std::vector<std::string> FxAssetWatcher::dependentsOf(const std::filesystem::path& file) const {
    std::scoped_lock lock(mutex_);
    const auto found = reverseDeps_.find(key(file));
    if (found == reverseDeps_.end())
        return {};
    std::vector<std::string> result(found->second.begin(), found->second.end());
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace dayo::fx
