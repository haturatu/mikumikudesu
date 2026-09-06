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
//
// Change detection is polling-based (mtime comparison via poll()); an
// inotify backend is an optional future optimization behind the same
// notifyChanged() entry point. poll() is cheap enough to call once per
// frame: it stats only tracked files.
class FxAssetWatcher {
  public:
    void addEffect(std::string effect, std::vector<std::filesystem::path> files);
    void removeEffect(const std::string& effect);
    // Manual notification used by tests and by the platform file watcher.
    // Returns the sorted list of effects that depend on the file.
    std::vector<std::string> notifyChanged(const std::filesystem::path& file);
    // Poll tracked files for mtime changes since the last snapshot.
    // Returns the sorted list of newly-dirtied effects.
    std::vector<std::string> poll();
    // Refresh the mtime baseline without dirtying (call after a reload).
    void snapshot();
    [[nodiscard]] std::vector<std::string> takeDirty();
    [[nodiscard]] bool hasDirty(const std::string& effect) const;
    void clear() noexcept;
    [[nodiscard]] std::size_t effectCount() const noexcept;
    // Reverse lookup without marking dirty (for diagnostics).
    [[nodiscard]] std::vector<std::string> dependentsOf(const std::filesystem::path& file) const;

  private:
    using TimePoint = std::filesystem::file_time_type;
    static std::string key(const std::filesystem::path& path);
    static TimePoint mtimeOf(const std::filesystem::path& path) noexcept;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> reverseDeps_; // file -> effects
    std::unordered_map<std::string, std::unordered_set<std::string>> forwardDeps_; // effect -> files
    std::unordered_map<std::string, TimePoint> mtimes_;                            // file -> last snapshotted mtime
    std::unordered_set<std::string> dirty_;
};

} // namespace dayo::fx
