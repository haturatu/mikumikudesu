#pragma once

#include "core/editor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dayo::editor {

// Stable key identity. Index references are never stored; only
// (track, stableId) pairs leave the editor. Frame order is applied
// exclusively at save/export time.
struct MotionKeyId {
    core::MotionTrack track{core::MotionTrack::bone};
    std::uint64_t stableId{};
    friend bool operator==(const MotionKeyId&, const MotionKeyId&) = default;
};

struct MotionKeyIdHash {
    [[nodiscard]] std::size_t operator()(const MotionKeyId& key) const noexcept {
        return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(key.track) * 0x9E3779B97F4A7C15ULL ^
                                           key.stableId);
    }
};

// Fingerprint used to re-resolve a stable id after sorts/inserts.
// Identity is (track, name); frame is mutable and updated via notifyMoved.
class StableIdTable {
  public:
    void rebuild(const core::MotionDocument& document);
    [[nodiscard]] MotionKeyId keyId(core::MotionTrack track, std::size_t index) const;
    [[nodiscard]] std::optional<std::size_t> resolve(const core::MotionDocument& document,
                                                     MotionKeyId id) const noexcept;
    void notifyMoved(const core::MotionDocument& document, const std::vector<MotionKeyId>& ids,
                     std::int64_t frameDelta);
    [[nodiscard]] std::size_t size() const noexcept {
        return fingerprints_.size();
    }

  private:
    struct Fingerprint {
        core::MotionTrack track{};
        std::string name;
        std::uint32_t frame{};
        std::size_t duplicateOrdinal{};
    };
    std::unordered_map<MotionKeyId, Fingerprint, MotionKeyIdHash> fingerprints_;
    std::unordered_map<MotionKeyId, std::size_t, MotionKeyIdHash> order_;
    std::uint64_t nextId_{1};
    static std::string keyName(const core::MotionDocument& document, core::MotionTrack track, std::size_t index);
    static std::uint32_t keyFrame(const core::MotionDocument& document, core::MotionTrack track, std::size_t index);
    static std::size_t trackSize(const core::MotionDocument& document, core::MotionTrack track);
};

} // namespace dayo::editor
