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

StableIdTable& StableIdTable::operator=(const StableIdTable& other) {
    if (this == &other)
        return *this;
    fingerprints_ = other.fingerprints_;
    order_ = other.order_;
    indices_ = other.indices_;
    nextId_ = std::max(nextId_, other.nextId_);
    return *this;
}
StableIdTable& StableIdTable::operator=(StableIdTable&& other) noexcept {
    if (this == &other)
        return *this;
    fingerprints_ = std::move(other.fingerprints_);
    order_ = std::move(other.order_);
    indices_ = std::move(other.indices_);
    nextId_ = std::max(nextId_, other.nextId_);
    return *this;
}
void StableIdTable::rebuild(const core::MotionDocument& document) {
    const auto identity = [](const Fingerprint& fp) {
        return std::to_string(static_cast<int>(fp.track)) + '\0' + fp.name + '\0' + std::to_string(fp.frame) + ':' +
               std::to_string(fp.duplicateOrdinal);
    };
    std::unordered_map<std::string, MotionKeyId> previous;
    previous.reserve(fingerprints_.size());
    for (const auto& [id, fp] : fingerprints_)
        previous.emplace(identity(fp), id);
    decltype(fingerprints_) rebuilt;
    decltype(order_) rebuiltOrder;
    rebuilt.reserve(fingerprints_.size());
    rebuiltOrder.reserve(fingerprints_.size());
    for (int value = 0; value < 6; ++value) {
        const auto track = static_cast<core::MotionTrack>(value);
        auto& indices = indices_[static_cast<std::size_t>(value)];
        indices.clear();
        indices.reserve(trackSize(document, track));
        std::unordered_map<std::string, std::size_t> ordinals;
        for (std::size_t index = 0; index < trackSize(document, track); ++index) {
            Fingerprint fp{track, keyName(document, track, index), keyFrame(document, track, index), 0};
            fp.duplicateOrdinal = ordinals[fp.name + '\0' + std::to_string(fp.frame)]++;
            const auto found = previous.find(identity(fp));
            if (found == previous.end() && nextId_ == 0)
                throw std::overflow_error("stable motion key id exhausted");
            const auto id = found == previous.end() ? MotionKeyId{track, nextId_++} : found->second;
            indices.push_back(id);
            rebuilt.emplace(id, std::move(fp));
            rebuiltOrder.emplace(id, index);
        }
    }
    fingerprints_.swap(rebuilt);
    order_.swap(rebuiltOrder);
}

MotionKeyId StableIdTable::keyId(core::MotionTrack track, std::size_t index) const {
    const auto value = static_cast<std::size_t>(track);
    if (value >= indices_.size() || index >= indices_[value].size())
        return {track, 0};
    return indices_[value][index];
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
    }

    // MotionEditor uses stable_sort, so keys that become equal retain their
    // pre-edit order. Rebuild ordinals from that order; otherwise two IDs can
    // acquire the same (name, frame, ordinal) fingerprint after a move.
    std::unordered_map<std::string, std::vector<MotionKeyId>> groups;
    for (const auto& [id, fingerprint] : fingerprints_) {
        const std::string key = std::to_string(static_cast<int>(fingerprint.track)) + '\0' + fingerprint.name + '\0' +
                                std::to_string(fingerprint.frame);
        groups[key].push_back(id);
    }
    for (auto& [key, group] : groups) {
        static_cast<void>(key);
        std::sort(group.begin(), group.end(), [&](MotionKeyId left, MotionKeyId right) {
            const auto leftOrder = order_.find(left);
            const auto rightOrder = order_.find(right);
            if (leftOrder == order_.end() || rightOrder == order_.end())
                return left.stableId < right.stableId;
            if (leftOrder->second != rightOrder->second)
                return leftOrder->second < rightOrder->second;
            return left.stableId < right.stableId;
        });
        if (group.empty())
            continue;

        const auto& fingerprint = fingerprints_.at(group.front());
        std::vector<std::size_t> currentIndices;
        const auto count = trackCount(document, fingerprint.track);
        for (std::size_t index = 0; index < count; ++index) {
            if (keyName(document, fingerprint.track, index) == fingerprint.name &&
                keyFrame(document, fingerprint.track, index) == fingerprint.frame)
                currentIndices.push_back(index);
        }
        for (std::size_t ordinal = 0; ordinal < group.size(); ++ordinal) {
            auto& current = fingerprints_.at(group[ordinal]);
            current.duplicateOrdinal = ordinal;
            if (ordinal < currentIndices.size())
                order_[group[ordinal]] = currentIndices[ordinal];
        }
    }
}

} // namespace dayo::editor
