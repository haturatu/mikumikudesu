#include "core/editor.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <utility>

namespace dayo::core {

void CommandHistory::execute(Scene& scene, std::unique_ptr<EditCommand> command) {
    if (!command)
        return;
    command->apply(scene);
    undo_.push_back(std::move(command));
    redo_.clear();
}

bool CommandHistory::undo(Scene& scene) {
    if (undo_.empty())
        return false;
    auto command = std::move(undo_.back());
    undo_.pop_back();
    command->undo(scene);
    redo_.push_back(std::move(command));
    return true;
}

bool CommandHistory::redo(Scene& scene) {
    if (redo_.empty())
        return false;
    auto command = std::move(redo_.back());
    redo_.pop_back();
    command->apply(scene);
    undo_.push_back(std::move(command));
    return true;
}

void CommandHistory::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

void SetFrameCommand::set(Scene& scene, float frame) {
    scene.setFrame(frame);
}

void SetFrameCommand::apply(Scene& scene) {
    set(scene, after_);
}
void SetFrameCommand::undo(Scene& scene) {
    set(scene, before_);
}

void SetRuntimeModeCommand::apply(Scene& scene) {
    scene.setRuntimeMode(after_);
}
void SetRuntimeModeCommand::undo(Scene& scene) {
    scene.setRuntimeMode(before_);
}

namespace {

template <typename Key> void sortKeys(std::vector<Key>& keys) {
    if constexpr (requires(const Key& key) { key.name; }) {
        std::stable_sort(keys.begin(), keys.end(), [](const Key& left, const Key& right) {
            return std::tie(left.name, left.frame) < std::tie(right.name, right.frame);
        });
    } else {
        std::stable_sort(keys.begin(), keys.end(),
                         [](const Key& left, const Key& right) { return left.frame < right.frame; });
    }
}

template <typename Key> void eraseIndices(std::vector<Key>& values, const std::vector<std::size_t>& indices) {
    std::size_t cursor = 0;
    values.erase(
        std::remove_if(values.begin(), values.end(),
                       [&](const Key&) { return std::binary_search(indices.begin(), indices.end(), cursor++); }),
        values.end());
}

std::uint32_t shiftedFrame(std::uint32_t frame, std::int64_t delta) {
    const auto value = static_cast<std::int64_t>(frame) + delta;
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(value, 0, static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())));
}

template <typename Key>
void appendSelected(std::vector<Key>& destination, const std::vector<Key>& source,
                    const std::vector<std::size_t>& indices, std::uint32_t& origin) {
    for (const auto index : indices)
        if (index < source.size()) {
            destination.push_back(source[index]);
            origin = std::min(origin, source[index].frame);
        }
}

template <typename Key>
void pasteTrack(std::vector<Key>& destination, const std::vector<Key>& source, std::int64_t delta, MotionTrack track,
                std::vector<MotionKeyRef>& pasted) {
    for (auto key : source) {
        key.frame = shiftedFrame(key.frame, delta);
        destination.push_back(std::move(key));
        pasted.push_back({track, destination.size() - 1});
    }
}

} // namespace

bool MotionClipboard::empty() const noexcept {
    return keys.bones.empty() && keys.morphs.empty() && keys.cameras.empty() && keys.lights.empty() &&
           keys.shadows.empty() && keys.ik.empty();
}

void MotionEditor::normalize(MotionDocument& document) {
    sortKeys(document.bones);
    sortKeys(document.morphs);
    sortKeys(document.cameras);
    sortKeys(document.lights);
    sortKeys(document.shadows);
    sortKeys(document.ik);
}

void MotionEditor::erase(MotionDocument& document, std::vector<MotionKeyRef> keys) {
    std::array<std::vector<std::size_t>, 6> indices;
    for (const auto& key : keys)
        indices[static_cast<std::size_t>(key.track)].push_back(key.index);
    for (auto& values : indices) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }
    eraseIndices(document.bones, indices[0]);
    eraseIndices(document.morphs, indices[1]);
    eraseIndices(document.cameras, indices[2]);
    eraseIndices(document.lights, indices[3]);
    eraseIndices(document.shadows, indices[4]);
    eraseIndices(document.ik, indices[5]);
}

void MotionEditor::move(MotionDocument& document, const std::vector<MotionKeyRef>& keys, std::int64_t frameDelta) {
    for (const auto& key : keys) {
        switch (key.track) {
        case MotionTrack::bone:
            if (key.index < document.bones.size())
                document.bones[key.index].frame = shiftedFrame(document.bones[key.index].frame, frameDelta);
            break;
        case MotionTrack::morph:
            if (key.index < document.morphs.size())
                document.morphs[key.index].frame = shiftedFrame(document.morphs[key.index].frame, frameDelta);
            break;
        case MotionTrack::camera:
            if (key.index < document.cameras.size())
                document.cameras[key.index].frame = shiftedFrame(document.cameras[key.index].frame, frameDelta);
            break;
        case MotionTrack::light:
            if (key.index < document.lights.size())
                document.lights[key.index].frame = shiftedFrame(document.lights[key.index].frame, frameDelta);
            break;
        case MotionTrack::shadow:
            if (key.index < document.shadows.size())
                document.shadows[key.index].frame = shiftedFrame(document.shadows[key.index].frame, frameDelta);
            break;
        case MotionTrack::ik:
            if (key.index < document.ik.size())
                document.ik[key.index].frame = shiftedFrame(document.ik[key.index].frame, frameDelta);
            break;
        }
    }
    normalize(document);
}

MotionClipboard MotionEditor::copy(const MotionDocument& document, const std::vector<MotionKeyRef>& keys) {
    MotionClipboard result;
    result.keys.interpolation = document.interpolation;
    result.originFrame = std::numeric_limits<std::uint32_t>::max();
    std::array<std::vector<std::size_t>, 6> indices;
    for (const auto& key : keys)
        indices[static_cast<std::size_t>(key.track)].push_back(key.index);
    appendSelected(result.keys.bones, document.bones, indices[0], result.originFrame);
    appendSelected(result.keys.morphs, document.morphs, indices[1], result.originFrame);
    appendSelected(result.keys.cameras, document.cameras, indices[2], result.originFrame);
    appendSelected(result.keys.lights, document.lights, indices[3], result.originFrame);
    appendSelected(result.keys.shadows, document.shadows, indices[4], result.originFrame);
    appendSelected(result.keys.ik, document.ik, indices[5], result.originFrame);
    if (result.empty())
        result.originFrame = 0;
    return result;
}

std::vector<MotionKeyRef> MotionEditor::paste(MotionDocument& document, const MotionClipboard& clipboard,
                                              std::uint32_t destinationFrame) {
    std::vector<MotionKeyRef> result;
    const auto delta = static_cast<std::int64_t>(destinationFrame) - clipboard.originFrame;
    pasteTrack(document.bones, clipboard.keys.bones, delta, MotionTrack::bone, result);
    pasteTrack(document.morphs, clipboard.keys.morphs, delta, MotionTrack::morph, result);
    pasteTrack(document.cameras, clipboard.keys.cameras, delta, MotionTrack::camera, result);
    pasteTrack(document.lights, clipboard.keys.lights, delta, MotionTrack::light, result);
    pasteTrack(document.shadows, clipboard.keys.shadows, delta, MotionTrack::shadow, result);
    pasteTrack(document.ik, clipboard.keys.ik, delta, MotionTrack::ik, result);
    normalize(document);
    return result;
}

void EditMotionCommand::apply(Scene& scene) {
    static_cast<void>(scene.replaceMotion(after_, target_, global_));
}
void EditMotionCommand::undo(Scene& scene) {
    static_cast<void>(scene.replaceMotion(before_, target_, global_));
}

} // namespace dayo::core
