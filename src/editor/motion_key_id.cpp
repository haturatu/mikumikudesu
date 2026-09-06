#include "editor/motion_key_id.hpp"

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
    fingerprints_.clear();
    order_.clear();
    nextId_ = 1;
    for (int trackValue = 0; trackValue < 6; ++trackValue) {
        const auto track = static_cast<core::MotionTrack>(trackValue);
        const auto count = trackSize(document, track);
        for (std::size_t index = 0; index < count; ++index) {
            const MotionKeyId id{track, nextId_++};
            fingerprints_.emplace(id, Fingerprint{track, keyName(document, track, index),
                                                  keyFrame(document, track, index)});
            order_.emplace(id.stableId, static_cast<std::uint64_t>(index));
        }
    }
}

MotionKeyId StableIdTable::keyId(core::MotionTrack track, std::size_t index) const {
    for (const auto& [id, fingerprint] : fingerprints_) {
        if (id.track != track)
            continue;
        const auto order = order_.find(id.stableId);
        if (order != order_.end() && order->second == static_cast<std::uint64_t>(index))
            return id;
    }
    return MotionKeyId{track, 0};
}

std::optional<std::size_t> StableIdTable::resolve(const core::MotionDocument& document, MotionKeyId id) const noexcept {
    const auto found = fingerprints_.find(id);
    if (found == fingerprints_.end() || id.stableId == 0)
        return std::nullopt;
    const auto& fingerprint = found->second;
    // Linear scan by (name, frame) survives frame-order sorts; indices are
    // never trusted across edits.
    const auto count = trackCount(document, id.track);
    for (std::size_t index = 0; index < count; ++index) {
        // NOTE: keyName/keyFrame are non-noexcept; resolve stays noexcept by
        // catching allocation failure as "not found".
        try {
            if (keyName(document, id.track, index) == fingerprint.name &&
                keyFrame(document, id.track, index) == fingerprint.frame)
                return index;
        } catch (...) {
            return std::nullopt;
        }
    }
    // Fallback: match by name only (frame may have been dragged without
    // notifyMoved in a skeleton caller).
    for (std::size_t index = 0; index < count; ++index) {
        try {
            if (keyName(document, id.track, index) == fingerprint.name)
                return index;
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void StableIdTable::notifyMoved(const core::MotionDocument& document, const std::vector<MotionKeyId>& ids,
                                std::int64_t frameDelta) {
    static_cast<void>(frameDelta);
    for (const auto& id : ids) {
        auto found = fingerprints_.find(id);
        if (found == fingerprints_.end())
            continue;
        const auto resolved = resolve(document, id);
        if (resolved.has_value()) {
            found->second.frame = keyFrame(document, id.track, *resolved);
            found->second.name = keyName(document, id.track, *resolved);
        }
    }
}

} // namespace dayo::editor
