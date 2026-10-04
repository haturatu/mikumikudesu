#include "editor/pose_binding.hpp"
#include "editor/editor_session.hpp"
#include "editor/pose_command.hpp"
#include "editor/viewport_math.hpp"

#include <algorithm>
#include <cmath>

namespace dayo::editor {
void BoneEditScratch::revert() noexcept {
    translation = baseTranslation;
    rotation = baseRotation;
    physics = basePhysics;
    modified = false;
}
void BoneEditScratch::initialize() noexcept {
    translation = {};
    rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    modified = true;
}
void PoseBinding::observe(core::ModelId model, const core::AnimatedModelFrame& frame) {
    evaluated_[model] = frame.bones;
}
const std::vector<core::AnimatedModelFrame::BoneTransform>* PoseBinding::poses(core::ModelId model) const noexcept {
    const auto found = evaluated_.find(model);
    return found == evaluated_.end() ? nullptr : &found->second;
}
void PoseBinding::synchronize(const core::Scene& scene, float frame) {
    const auto* model = scene.selectedModel();
    const auto target = model ? model->id : 0;
    if (target == target_ && frame == frame_ && revision_ == scene.motionRevision())
        return;
    if (target != target_) {
        selectedBones_.clear();
        activeBone_ = -1;
    }
    target_ = target;
    frame_ = frame;
    revision_ = scene.motionRevision();
    edits_.clear();
    const auto* evaluated = poses(target);
    if (!model || !evaluated)
        return;
    std::unordered_map<std::string, const core::VmdBoneKey*> precedingKeys;
    if (model->motion)
        for (const auto& key : model->motion->bones)
            if (static_cast<float>(key.frame) <= frame) {
                auto& previous = precedingKeys[key.name];
                if (!previous || key.frame >= previous->frame)
                    previous = &key;
            }
    for (std::size_t index = 0; index < evaluated->size(); ++index) {
        const auto& pose = (*evaluated)[index];
        BoneEditScratch edit;
        edit.baseTranslation = pose.inputTranslation;
        edit.baseRotation = pose.inputRotation;
        edit.revert();
        if (model->motion && index < model->model->bones.size()) {
            const auto found = precedingKeys.find(model->model->bones[index].name);
            if (found != precedingKeys.end())
                edit.physics = found->second->physics;
            edit.basePhysics = edit.physics;
        }
        edits_.emplace(static_cast<int>(index), edit);
    }
    if (activeBone_ < 0 && !evaluated->empty())
        selectBone(0);
}
void PoseBinding::selectBone(int bone, bool add, bool toggle) {
    if (!edits_.contains(bone))
        return;
    if (!add && !toggle)
        selectedBones_.clear();
    const auto found = std::ranges::find(selectedBones_, bone);
    if (toggle && found != selectedBones_.end())
        selectedBones_.erase(found);
    else if (found == selectedBones_.end())
        selectedBones_.push_back(bone);
    activeBone_ = selectedBones_.empty() ? -1 : selectedBones_.back();
}
void PoseBinding::setBones(std::vector<int> bones) {
    selectedBones_.clear();
    for (const auto bone : bones)
        if (edits_.contains(bone) && std::ranges::find(selectedBones_, bone) == selectedBones_.end())
            selectedBones_.push_back(bone);
    activeBone_ = selectedBones_.empty() ? -1 : selectedBones_.back();
}
BoneEditScratch* PoseBinding::activeEdit() noexcept {
    const auto found = edits_.find(activeBone_);
    return found == edits_.end() ? nullptr : &found->second;
}
std::vector<mmd::BoneOverride> PoseBinding::overrides(core::ModelId model, float frame, std::uint64_t revision) const {
    std::vector<mmd::BoneOverride> result;
    if (model != target_ || frame != frame_ || revision != revision_)
        return result;
    for (const auto& [index, edit] : edits_)
        if (edit.modified)
            result.push_back({static_cast<std::size_t>(index), edit.translation, edit.rotation, edit.physics});
    return result;
}
void PoseBinding::registerBones(EditorSession& session, std::uint32_t frame) {
    auto* scene = session.scene();
    auto* model = scene ? scene->model(target_) : nullptr;
    if (!model || !model->model)
        return;
    const auto before = model->motion ? *model->motion : core::VmdMotion{};
    auto document = core::toMotionDocument(before);
    std::vector<std::string> names;
    for (const auto bone : selectedBones_) {
        if (bone < 0 || static_cast<std::size_t>(bone) >= model->model->bones.size())
            continue;
        const auto& name = model->model->bones[static_cast<std::size_t>(bone)].name;
        names.push_back(name);
        const auto& edit = edits_.at(bone);
        core::VmdBoneKey key;
        key.name = name;
        key.frame = frame;
        key.translation = edit.translation;
        key.rotation = multiplyRotation(edit.rotation, {0.0F, 0.0F, 0.0F, 1.0F});
        key.physics = edit.physics;
        const auto found = std::ranges::find_if(
            document.bones, [&](const auto& item) { return item.name == name && item.frame == frame; });
        if (found != document.bones.end()) {
            key.interpolation = found->interpolation;
            key.methods = found->methods;
        }
        std::erase_if(document.bones, [&](const auto& item) { return item.name == name && item.frame == frame; });
        document.bones.push_back(std::move(key));
    }
    core::MotionEditor::normalize(document);
    if (model->pose) {
        session.history()->execute(*scene, std::make_unique<RegisterPoseCommand>(
                                               *model, core::toVmdMotion(std::move(document), before.modelName),
                                               std::move(names), &session.stableIds(target_, false)));
    } else
        session.operations().push(ReplaceMotionOperation{
            target_, false, core::toVmdMotion(std::move(document), before.modelName), "Register bone keys"});
}
void PoseBinding::translateSelected(core::Float3 delta, const core::ModelInstance& model, bool world) {
    const auto* evaluated = poses(model.id);
    if (!evaluated)
        return;
    for (const auto bone : selectedBones_) {
        auto localDelta = delta;
        const auto parent = model.model->bones[static_cast<std::size_t>(bone)].parent;
        if (world && parent >= 0 && static_cast<std::size_t>(parent) < evaluated->size())
            localDelta = rotatePoint(inverseRotation((*evaluated)[static_cast<std::size_t>(parent)].rotation), delta);
        auto& edit = edits_.at(bone);
        for (std::size_t axis = 0; axis < 3; ++axis)
            edit.translation[axis] += localDelta[axis];
        edit.modified = true;
    }
}
void PoseBinding::rotateSelected(core::Float4 delta, const core::ModelInstance& model, bool world) {
    const auto* evaluated = poses(model.id);
    if (!evaluated)
        return;
    for (const auto bone : selectedBones_) {
        auto localDelta = delta;
        const auto parent = model.model->bones[static_cast<std::size_t>(bone)].parent;
        if (world && parent >= 0 && static_cast<std::size_t>(parent) < evaluated->size()) {
            const auto q = (*evaluated)[static_cast<std::size_t>(parent)].rotation;
            localDelta = multiplyRotation(multiplyRotation(inverseRotation(q), delta), q);
        }
        auto& edit = edits_.at(bone);
        edit.rotation = world ? multiplyRotation(localDelta, edit.rotation) : multiplyRotation(edit.rotation, delta);
        edit.modified = true;
    }
}
const BoneEditScratch* PoseBinding::edit(int bone) const noexcept {
    const auto found = edits_.find(bone);
    return found == edits_.end() ? nullptr : &found->second;
}
void PoseBinding::setPhysicsSelected(bool enabled) noexcept {
    for (const auto bone : selectedBones_) {
        auto& value = edits_.at(bone);
        value.physics = enabled;
        value.modified = true;
    }
}
void PoseBinding::revertSelected() noexcept {
    for (const auto bone : selectedBones_)
        if (auto found = edits_.find(bone); found != edits_.end())
            found->second.revert();
}
void PoseBinding::initializeSelected() noexcept {
    for (const auto bone : selectedBones_)
        if (auto found = edits_.find(bone); found != edits_.end())
            found->second.initialize();
}
bool PoseBinding::modified() const noexcept {
    return std::ranges::any_of(edits_, [](const auto& entry) { return entry.second.modified; });
}
} // namespace dayo::editor
