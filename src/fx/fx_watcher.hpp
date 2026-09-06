#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dayo::fx {

// File -> effect reverse dependency tracker. Each effect declares the files
// it was linked from; on filesystem change only the related effects are
// recompiled. Forward map (effect -> files) is kept for eviction.
class FxAssetWatcher {
  public:
    void addEffect(std::string effect, std::vector<std::filesystem::path> files);
    void removeEffect(const std::string& effect);
    // Captures current mtimes for all tracked files without marking effects dirty.
    void snapshot();
    // Returns effects affected by files whose mtime changed since snapshot().
    std::vector<std::string> poll();
    // Manual notification used by tests and by the platform file watcher.
    // Returns the sorted list of effects that depend on the file.
    std::vector<std::string> notifyChanged(const std::filesystem::path& file);
    [[nodiscard]] std::vector<std::string> takeDirty();
    [[nodiscard]] bool hasDirty(const std::string& effect) const;
    void clear() noexcept;
    [[nodiscard]] std::size_t effectCount() const noexcept;
    // Reverse lookup without marking dirty (for diagnostics).
    [[nodiscard]] std::vector<std::string> dependentsOf(const std::filesystem::path& file) const;

  private:
    static std::string key(const std::filesystem::path& path);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> reverseDeps_; // file -> effects
    std::unordered_map<std::string, std::unordered_set<std::string>> forwardDeps_; // effect -> files
    std::unordered_set<std::string> dirty_;
    std::unordered_map<std::string, std::filesystem::file_time_type> snapshots_;
};

} // namespace dayo::fx
