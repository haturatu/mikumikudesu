#include "editor/camera_recording.hpp"
#include <algorithm>
#include <utility>
namespace dayo::editor {
CameraRecording::CameraRecording(core::Scene& scene, core::CommandHistory& history, StableIdTable& ids,
                                 std::int64_t start, std::int64_t end)
    : scene_(scene), ids_(ids), transaction_(scene, history, 0, true, "Record camera range", &ids),
      start_(std::max<std::int64_t>(0, start)), end_(std::max(start_, end)) {
    auto motion = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
    std::erase_if(motion.cameras, [&](const auto& key) {
        return static_cast<std::int64_t>(key.frame) >= start_ && static_cast<std::int64_t>(key.frame) <= end_;
    });
    transaction_.dragTo(std::move(motion));
    scene_.setTimelineDuration(std::max(scene_.timeline().duration, static_cast<float>(end_)));
}
bool CameraRecording::update(core::VmdCameraKey key) {
    const bool requested = std::exchange(requested_, false);
    if (!requested || static_cast<std::int64_t>(key.frame) < start_ || static_cast<std::int64_t>(key.frame) > end_)
        return false;
    auto motion = scene_.cameraMotion() ? *scene_.cameraMotion() : core::VmdMotion{};
    std::erase_if(motion.cameras, [&](const auto& item) { return item.frame == key.frame; });
    motion.cameras.push_back(key);
    auto document = core::toMotionDocument(motion);
    core::MotionEditor::normalize(document);
    transaction_.dragTo(core::toVmdMotion(document, motion.modelName));
    scene_.setTimelineDuration(std::max(scene_.timeline().duration, static_cast<float>(end_)));
    ids_.rebuild(document);
    const auto found = std::ranges::find(document.cameras, key.frame, &core::VmdCameraKey::frame);
    const auto index = static_cast<std::size_t>(std::distance(document.cameras.begin(), found));
    const auto id = ids_.keyId(core::MotionTrack::camera, index);
    if (std::ranges::find(recorded_, id) == recorded_.end())
        recorded_.push_back(id);
    return true;
}
std::vector<MotionKeyId> CameraRecording::finish() {
    if (recorded_.empty())
        transaction_.rollback();
    else
        transaction_.commit();
    return recorded_;
}
} // namespace dayo::editor
