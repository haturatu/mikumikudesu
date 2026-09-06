#include "editor/motion_key_id.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace dayo::editor {

namespace {

std::size_t trackCount(const core::MotionDocument& document, core::MotionTrack track) {
    switch (track) {
    case core::MotionTrack::bone:
        return document.bones.size();
    case core::MotionTrack::morph:
        return document.morphs.size();
    case core::MotionTrack::camera:
        return document.cameras.size();
    case core::MotionTrack::light:
        return document.lights.size();
    case core::MotionTrack::shadow:
        return document.shadows.size();
    case core::MotionTrack::ik:
        return document.ik.size();
    }
    return 0;
}

} // namespace

std::string StableIdTable::keyName(const core::MotionDocument& document, core::MotionTrack track, std::size_t index) {
    switch (track) {
    case core::MotionTrack::bone:
        return index < document.bones.size() ? document.bones[index].name : std::string{};
    case core::MotionTrack::morph:
        return index < document.morphs.size() ? document.morphs[index].name : std::string{};
    case core::MotionTrack::camera:
        return "camera";
    case core::MotionTrack::light:
        return "light";
    case core::MotionTrack::shadow:
        return "shadow";
    case core::MotionTrack::ik:
        return "ik";
    }
    return {};
}

std::uint32_t StableIdTable::keyFrame(const core::MotionDocument& document, core::MotionTrack track,
                                      std::size_t index) {
    switch (track) {
    case core::MotionTrack::bone:
        return index < document.bones.size() ? document.bones[index].frame : 0U;
    case core::MotionTrack::morph:
        return index < document.morphs.size() ? document.morphs[index].frame : 0U;
    case core::MotionTrack::camera:
        return index < document.cameras.size() ? document.cameras[index].frame : 0U;
    case core::MotionTrack::light:
        return index < document.lights.size() ? document.lights[index].frame : 0U;
    case core::MotionTrack::shadow:
        return index < document.shadows.size() ? document.shadows[index].frame : 0U;
    case core::MotionTrack::ik:
        return index < document.ik.size() ? document.ik[index].frame : 0U;
    }
    return 0U;
}

std::size_t StableIdTable::trackSize(const core::MotionDocument& document, core::MotionTrack track) {
    return trackCount(document, track);
}

void StableIdTable::rebuild(const core::MotionDocument& document) {
    const auto previous = fingerprints_;
    std::vector<std::pair<MotionKeyId, Fingerprint>> old;
    old.reserve(previous.size());
    for (const auto& entry : previous)
        old.emplace_back(entry);
    std::vector<bool> used(old.size(), false);

    std::unordered_map<MotionKeyId, Fingerprint, MotionKeyIdHash> rebuilt;
    std::unordered_map<MotionKeyId, std::size_t, MotionKeyIdHash> rebuiltOrder;
    rebuilt.reserve(document.bones.size() + document.morphs.size() + document.cameras.size() + document.lights.size() +
                    document.shadows.size() + document.ik.size());
    rebuiltOrder.reserve(rebuilt.size());

    for (int trackValue = 0; trackValue < 6; ++trackValue) {
        const auto track = static_cast<core::MotionTrack>(trackValue);
        const auto count = trackSize(document, track);
        std::unordered_map<std::string, std::size_t> ordinals;
        for (std::size_t index = 0; index < count; ++index) {
            const Fingerprint candidate{track, keyName(document, track, index), keyFrame(document, track, index), 0};
            const std::string ordinalKey = candidate.name + '\0' + std::to_string(candidate.frame);
            const std::size_t duplicateOrdinal = ordinals[ordinalKey]++;
            Fingerprint current = candidate;
            current.duplicateOrdinal = duplicateOrdinal;

            std::optional<std::size_t> matched;
            for (std::size_t oldIndex = 0; oldIndex < old.size(); ++oldIndex) {
                if (used[oldIndex] || old[oldIndex].second.track != current.track ||
                    old[oldIndex].second.name != current.name || old[oldIndex].second.frame != current.frame ||
                    old[oldIndex].second.duplicateOrdinal != current.duplicateOrdinal)
                    continue;
                matched = oldIndex;
                break;
            }
            // A caller may rebuild after changing a frame without calling
            // notifyMoved. Preserve a unique named key in that case too;
            // duplicate names still require the explicit ordinal identity.
            if (!matched.has_value()) {
                std::size_t currentNameCount = 0;
                for (std::size_t other = 0; other < count; ++other)
                    currentNameCount += keyName(document, track, other) == current.name ? 1U : 0U;
                std::size_t oldNameCount = 0;
                std::size_t oldCandidate = 0;
                for (std::size_t oldIndex = 0; oldIndex < old.size(); ++oldIndex) {
                    if (used[oldIndex] || old[oldIndex].second.track != current.track ||
                        old[oldIndex].second.name != current.name)
                        continue;
                    ++oldNameCount;
                    oldCandidate = oldIndex;
                }
                if (currentNameCount == 1 && oldNameCount == 1)
                    matched = oldCandidate;
            }

            MotionKeyId id;
            if (matched.has_value()) {
                used[*matched] = true;
                id = old[*matched].first;
            } else {
                if (nextId_ == 0)
                    throw std::overflow_error("stable motion key id exhausted");
                id = MotionKeyId{track, nextId_++};
            }
            rebuilt.emplace(id, std::move(current));
            rebuiltOrder.emplace(id, index);
        }
    }
    fingerprints_.swap(rebuilt);
    order_.clear();
    order_.swap(rebuiltOrder);
}

MotionKeyId StableIdTable::keyId(core::MotionTrack track, std::size_t index) const {
    for (const auto& [id, fingerprint] : fingerprints_) {
        static_cast<void>(fingerprint);
        if (id.track != track)
            continue;
        const auto order = order_.find(id);
        if (order != order_.end() && order->second == index)
            return id;
    }
    return MotionKeyId{track, 0};
}

std::optional<std::size_t> StableIdTable::resolve(const core::MotionDocument& document, MotionKeyId id) const noexcept {
    const auto found = fingerprints_.find(id);
    if (found == fingerprints_.end() || id.stableId == 0)
        return std::nullopt;
    const auto& fingerprint = found->second;
    // Linear scan by (name, frame, duplicate ordinal) survives frame-order
    // sorts; indices are never trusted across edits.
    std::size_t duplicateOrdinal = 0;
    const auto count = trackCount(document, id.track);
    for (std::size_t index = 0; index < count; ++index) {
        // NOTE: keyName/keyFrame are non-noexcept; resolve stays noexcept by
        // catching allocation failure as "not found".
        try {
            if (keyName(document, id.track, index) == fingerprint.name &&
                keyFrame(document, id.track, index) == fingerprint.frame) {
                const auto currentOrdinal = duplicateOrdinal++;
                if (currentOrdinal == fingerprint.duplicateOrdinal)
                    return index;
            }
        } catch (...) {
            return std::nullopt;
        }
    }
    // Fallback: match by name only when that name is unique. Never guess
    // among duplicate names after an edit, because doing so can select a
    // different key than the stable id identifies.
    std::optional<std::size_t> uniqueName;
    for (std::size_t index = 0; index < count; ++index) {
        try {
            if (keyName(document, id.track, index) == fingerprint.name) {
                if (uniqueName.has_value())
                    return std::nullopt;
                uniqueName = index;
            }
        } catch (...) {
            return std::nullopt;
        }
    }
    return uniqueName;
}

void StableIdTable::notifyMoved(const core::MotionDocument& document, const std::vector<MotionKeyId>& ids,
                                std::int64_t frameDelta) {
    const auto shiftedFrame = [](std::uint32_t frame, std::int64_t delta) {
        const auto value = static_cast<std::int64_t>(frame) + delta;
        return static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(value, 0, static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())));
    };
    for (const auto& id : ids) {
        auto found = fingerprints_.find(id);
        if (found == fingerprints_.end())
            continue;
        found->second.frame = shiftedFrame(found->second.frame, frameDelta);
        const auto resolved = resolve(document, id);
        if (resolved.has_value()) {
            found->second.name = keyName(document, id.track, *resolved);
        }
    }
}

} // namespace dayo::editor
