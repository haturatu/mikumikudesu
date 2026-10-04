#include "editor/model_commands.hpp"
#include <algorithm>
#include <stdexcept>
namespace dayo::editor {
core::VmdMotion remapMotionModels(core::VmdMotion motion,
                                  const std::vector<std::pair<core::ModelId, core::ModelId>>& modelIds) {
    const auto remap = [&](std::int32_t id) {
        if (id < 0)
            return id;
        const auto found = std::ranges::find(modelIds, static_cast<core::ModelId>(id),
                                             &std::pair<core::ModelId, core::ModelId>::first);
        return found == modelIds.end() ? -1 : static_cast<std::int32_t>(found->second);
    };
    for (auto& key : motion.cameras)
        key.parentModel = remap(key.parentModel);
    for (auto& key : motion.externalParents)
        key.parentModel = remap(key.parentModel);
    return motion;
}
DeleteModelCommand::DeleteModelCommand(const core::Scene& scene, core::ModelId id)
    : effects_(scene.effects()), id_(id), selection_(scene.selectedModelId()), links_(scene.externalParents()) {
    const auto& models = scene.models();
    const auto found = std::ranges::find_if(models, [id](const auto& model) { return model.id == id; });
    index_ = static_cast<std::size_t>(std::distance(models.begin(), found));
}
void DeleteModelCommand::apply(core::Scene& scene) {
    removed_ = scene.takeModel(id_);
}
void DeleteModelCommand::undo(core::Scene& scene) {
    if (!removed_)
        return;
    scene.restoreModel(std::move(*removed_), index_);
    removed_.reset();
    if (!scene.setExternalParents(links_))
        throw std::runtime_error("cannot restore external parents");
    scene.restoreEffects(effects_);
    scene.selectModel(selection_);
}
SetModelOrdersCommand::SetModelOrdersCommand(const core::Scene& scene, const std::vector<core::ModelId>& ids, int stage)
    : ids_(ids) {
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto* model = scene.model(ids[index]);
        if (!model)
            throw std::invalid_argument("unknown model");
        before_.push_back(model->order);
        auto order = model->order;
        const auto rank = static_cast<std::int32_t>(index);
        switch (stage) {
        case 0:
            order.motion = rank;
            break;
        case 1:
            order.deform = rank;
            break;
        case 2:
            order.postprocess = rank;
            break;
        case 3:
            order.raster = rank;
            break;
        default:
            throw std::invalid_argument("unknown model stage");
        }
        after_.push_back(order);
    }
}
void SetModelOrdersCommand::set(core::Scene& scene, bool after) {
    const auto& orders = after ? after_ : before_;
    for (std::size_t index = 0; index < ids_.size(); ++index)
        if (auto* model = scene.model(ids_[index]))
            model->order = orders[index];
    scene.markDirty(core::DirtyFlag::geometry | core::DirtyFlag::effect);
}
void SetModelOrdersCommand::apply(core::Scene& scene) {
    set(scene, true);
}
void SetModelOrdersCommand::undo(core::Scene& scene) {
    set(scene, false);
}
ExternalParentsCommand::ExternalParentsCommand(const core::Scene& scene, std::vector<core::ExternalParentLink> links,
                                               std::uint32_t frame)
    : before_(scene.externalParents()), after_(std::move(links)) {
    const auto effective = scene.effectiveExternalParents(static_cast<float>(frame));
    for (const auto& model : scene.models()) {
        std::vector<std::string> children;
        for (const auto& link : effective)
            if (link.childModel == model.id)
                children.push_back(link.childBone);
        for (const auto& link : before_)
            if (link.childModel == model.id && std::ranges::find(children, link.childBone) == children.end())
                children.push_back(link.childBone);
        for (const auto& link : after_)
            if (link.childModel == model.id && std::ranges::find(children, link.childBone) == children.end())
                children.push_back(link.childBone);
        if (children.empty())
            continue;
        auto motion = model.motion ? *model.motion : core::VmdMotion{};
        auto document = core::toMotionDocument(motion);
        for (const auto& child : children) {
            std::erase_if(document.externalParents,
                          [&](const auto& key) { return key.frame == frame && key.childBone == child; });
            const auto found = std::ranges::find_if(
                after_, [&](const auto& link) { return link.childModel == model.id && link.childBone == child; });
            if (found == after_.end())
                document.externalParents.push_back({frame, -1, {}, child});
            else
                document.externalParents.push_back(
                    {frame, static_cast<std::int32_t>(found->parentModel), found->parentBone, child});
        }
        core::MotionEditor::normalize(document);
        motions_.push_back({model.id, motion, core::toVmdMotion(std::move(document), motion.modelName)});
    }
}
void ExternalParentsCommand::apply(core::Scene& scene) {
    std::string error;
    if (!scene.setExternalParents(after_, &error))
        throw std::invalid_argument(error);
    for (const auto& motion : motions_)
        scene.replaceMotion(motion.after, motion.target, false);
}
void ExternalParentsCommand::undo(core::Scene& scene) {
    if (!scene.setExternalParents(before_))
        throw std::runtime_error("cannot restore external parents");
    for (const auto& motion : motions_)
        scene.replaceMotion(motion.before, motion.target, false);
}
} // namespace dayo::editor
