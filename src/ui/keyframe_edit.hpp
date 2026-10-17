#pragma once

#include "core/editor.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <span>

namespace dayo::ui {

template <typename Motion, typename Visitor> bool visitKey(Motion& motion, core::MotionKeyRef ref, Visitor visitor) {
    const auto visit = [&](auto& keys) {
        if (ref.index >= keys.size())
            return false;
        visitor(keys[ref.index]);
        return true;
    };
    switch (ref.track) {
    case core::MotionTrack::bone:
        return visit(motion.bones);
    case core::MotionTrack::morph:
        return visit(motion.morphs);
    case core::MotionTrack::camera:
        return visit(motion.cameras);
    case core::MotionTrack::light:
        return visit(motion.lights);
    case core::MotionTrack::shadow:
        return visit(motion.shadows);
    case core::MotionTrack::ik:
        return visit(motion.ik);
    }
    return false;
}

inline std::optional<std::uint32_t> keyFrame(const core::VmdMotion& motion, core::MotionKeyRef ref) {
    std::optional<std::uint32_t> frame;
    visitKey(motion, ref, [&](const auto& key) { frame = key.frame; });
    return frame;
}

inline void updateMotionDuration(core::VmdMotion& motion) {
    motion.lastFrame = 0;
    const auto scan = [&](const auto& keys) {
        for (const auto& key : keys)
            motion.lastFrame = std::max(motion.lastFrame, key.frame);
    };
    scan(motion.bones);
    scan(motion.morphs);
    scan(motion.cameras);
    scan(motion.lights);
    scan(motion.shadows);
    scan(motion.ik);
    scan(motion.externalParents);
    scan(motion.gravity);
}

// Keep the selection's spacing at both bounds. Preserve vector order so refs
// continue to identify the same keys throughout an edit and its undo snapshot.
inline std::int64_t moveKeyframes(core::VmdMotion& motion, std::span<const core::MotionKeyRef> selection,
                                  std::int64_t delta) {
    std::uint32_t first = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t last = 0;
    bool found = false;
    for (const auto ref : selection) {
        if (const auto frame = keyFrame(motion, ref)) {
            first = std::min(first, *frame);
            last = std::max(last, *frame);
            found = true;
        }
    }
    if (!found)
        return 0;
    delta = std::clamp(delta, -static_cast<std::int64_t>(first),
                       static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) - last);
    if (delta == 0)
        return 0;
    // Ignore repeated references rather than applying the offset twice.
    std::vector<core::MotionKeyRef> moved;
    for (const auto ref : selection) {
        if (std::find(moved.begin(), moved.end(), ref) != moved.end())
            continue;
        visitKey(motion, ref, [&](auto& key) {
            key.frame = static_cast<std::uint32_t>(static_cast<std::int64_t>(key.frame) + delta);
        });
        moved.push_back(ref);
    }
    updateMotionDuration(motion);
    return delta;
}

} // namespace dayo::ui
