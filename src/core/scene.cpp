#include "core/scene.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <stdexcept>

namespace dayo::core {

Scene::Scene() = default;
Scene::~Scene() = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

ModelId Scene::addModel(const std::filesystem::path& path) {
    ModelInstance instance;
    instance.id = nextId_++;
    instance.sourcePath = std::filesystem::absolute(path).lexically_normal();
    instance.model = std::make_shared<PmxModel>(loadPmxModel(path));
    instance.animator = std::make_unique<MmdAnimator>(*instance.model);
    instance.physics = std::make_unique<MmdPhysics>(*instance.model);
    instance.softBody = std::make_unique<SoftBodySimulation>(*instance.model);
    instance.normalization = previewNormalization(*instance.model);
    instance.displayName = instance.model->metadata.modelName.empty()
        ? path.filename().string() : instance.model->metadata.modelName;
    refreshModelResources(instance);
    models_.push_back(std::move(instance));
    selectedModel_ = models_.back().id;
    timeline_.duration = std::max(timeline_.duration, 0.0F);
    markDirty(DirtyFlag::geometry | DirtyFlag::material);
    return selectedModel_;
}

bool Scene::removeModel(ModelId id) {
    const auto found = std::find_if(models_.begin(), models_.end(), [id](const auto& item) { return item.id == id; });
    if (found == models_.end()) return false;
    models_.erase(found);
    if (selectedModel_ == id) selectedModel_ = models_.empty() ? 0 : models_.front().id;
    markDirty(DirtyFlag::geometry | DirtyFlag::material);
    return true;
}

void Scene::clearModels() {
    models_.clear();
    selectedModel_ = 0;
    markDirty(DirtyFlag::geometry | DirtyFlag::material);
}

ModelInstance* Scene::model(ModelId id) noexcept {
    const auto found = std::find_if(models_.begin(), models_.end(), [id](auto& item) { return item.id == id; });
    return found == models_.end() ? nullptr : std::addressof(*found);
}

const ModelInstance* Scene::model(ModelId id) const noexcept {
    const auto found = std::find_if(models_.begin(), models_.end(), [id](const auto& item) { return item.id == id; });
    return found == models_.end() ? nullptr : std::addressof(*found);
}

ModelInstance* Scene::selectedModel() noexcept { return model(selectedModel_); }
const ModelInstance* Scene::selectedModel() const noexcept { return model(selectedModel_); }

void Scene::selectModel(ModelId id) noexcept {
    if (model(id) != nullptr) selectedModel_ = id;
}

ModelId Scene::targetModel(ModelId requested) const noexcept {
    if (requested != 0 && model(requested) != nullptr) return requested;
    return selectedModel_;
}

bool Scene::addExternalParent(ExternalParentLink link, std::string* error) {
    const auto* parent = model(link.parentModel);
    const auto* child = model(link.childModel);
    if (parent == nullptr || child == nullptr || link.parentBone.empty() || link.childBone.empty()) {
        if (error != nullptr) *error = "external parent references an unknown model or bone";
        return false;
    }
    const auto boneExists = [](const ModelInstance& instance, const std::string& name) {
        return std::any_of(instance.model->bones.begin(), instance.model->bones.end(),
                           [&name](const auto& bone) { return bone.name == name; });
    };
    if (!boneExists(*parent, link.parentBone) || !boneExists(*child, link.childBone)) {
        if (error != nullptr) *error = "external parent references an unknown bone";
        return false;
    }
    externalParents_.push_back(std::move(link));
    if (hasExternalParentCycle()) {
        externalParents_.pop_back();
        if (error != nullptr) *error = "external parent cycle detected";
        return false;
    }
    timeline_.externalParents = externalParents_;
    markDirty(DirtyFlag::geometry | DirtyFlag::camera);
    return true;
}

bool Scene::hasExternalParentCycle() const noexcept {
    std::unordered_map<ModelId, std::vector<ModelId>> edges;
    for (const auto& link : externalParents_) edges[link.childModel].push_back(link.parentModel);
    std::unordered_map<ModelId, std::uint8_t> state;
    std::function<bool(ModelId)> visit = [&](ModelId id) {
        if (state[id] == 1) return true;
        if (state[id] == 2) return false;
        state[id] = 1;
        for (const auto parent : edges[id]) if (visit(parent)) return true;
        state[id] = 2;
        return false;
    };
    for (const auto& item : models_) if (visit(item.id)) return true;
    return false;
}

bool Scene::solveExternalParents() {
    if (hasExternalParentCycle()) return false;
    for (const auto& link : externalParents_) {
        auto* parent = model(link.parentModel);
        auto* child = model(link.childModel);
        if (parent == nullptr || child == nullptr) continue;
        const auto found = std::find_if(parent->model->bones.begin(), parent->model->bones.end(),
                                        [&link](const auto& bone) { return bone.name == link.parentBone; });
        if (found == parent->model->bones.end()) continue;
        for (std::size_t axis = 0; axis < 3; ++axis) child->worldPosition[axis] = parent->worldPosition[axis] + found->position[axis];
        child->worldRotation = parent->worldRotation;
    }
    if (!externalParents_.empty()) markDirty(DirtyFlag::geometry | DirtyFlag::camera);
    return true;
}

PhysicsSettings Scene::evaluatePhysicsSettings(float frame) const noexcept {
    if (timeline_.gravity.empty()) return physicsSettings_;
    const auto found = std::upper_bound(timeline_.gravity.begin(), timeline_.gravity.end(), frame,
        [](float value, const auto& key) { return value < static_cast<float>(key.first); });
    if (found == timeline_.gravity.begin()) return found->second;
    return std::prev(found)->second;
}

void Scene::attachMotion(const std::filesystem::path& path, ModelId target) {
    auto motion = std::make_unique<VmdMotion>(loadVmd(path));
    // A camera/light-only VMD is global even when a model is selected. Model
    // motion remains the default for files containing bone or morph tracks.
    const bool cameraOnly = motion->bones.empty() && motion->morphs.empty()
        && (!motion->cameras.empty() || !motion->lights.empty());
    auto* destination = cameraOnly ? nullptr : model(targetModel(target));
    if (destination != nullptr) {
        destination->motion = std::move(motion);
        destination->animator->setMotion(destination->motion.get());
        timeline_.duration = std::max(timeline_.duration, static_cast<float>(destination->motion->lastFrame));
    } else {
        // Camera/light VMDs are still useful without a model.
        cameraMotion_ = std::move(motion);
        timeline_.camera = cameraMotion_->cameras;
        timeline_.light = cameraMotion_->lights;
        timeline_.shadow = cameraMotion_->shadows;
        timeline_.ik = cameraMotion_->ik;
        timeline_.duration = std::max(timeline_.duration, static_cast<float>(cameraMotion_->lastFrame));
    }
    markDirty(DirtyFlag::geometry | DirtyFlag::camera | DirtyFlag::lighting);
}

void Scene::attachPose(const std::filesystem::path& path, ModelId target) {
    auto* destination = model(targetModel(target));
    if (destination == nullptr) throw std::runtime_error("VPD requires a model target");
    destination->pose = std::make_unique<VpdPose>(loadVpd(path));
    destination->animator->setPose(destination->pose.get());
    markDirty(DirtyFlag::geometry);
}

void Scene::setBackgroundImage(const std::filesystem::path& path) {
    background_.image = loadImageRgba8(path);
    background_.imagePath = std::filesystem::absolute(path).lexically_normal();
    background_.videoPath.reset();
    background_.screenSource = ScreenTextureSource::backgroundImage;
    markDirty(DirtyFlag::background);
}

void Scene::setBackgroundVideo(const std::filesystem::path& path) {
    background_.videoPath = std::filesystem::absolute(path).lexically_normal();
    background_.screenSource = ScreenTextureSource::backgroundVideo;
    markDirty(DirtyFlag::background);
}

void Scene::setMedia(const std::filesystem::path& path) {
    media_.emplace(path);
    if (media_->info().hasVideo) setBackgroundVideo(path);
    else background_.screenSource = ScreenTextureSource::previousFrame;
    markDirty(DirtyFlag::background);
}

void Scene::clearBackground() {
    background_ = {};
    markDirty(DirtyFlag::background);
}

void Scene::clearMedia() {
    media_.reset();
    markDirty(DirtyFlag::background);
}

MediaFile* Scene::media() noexcept { return media_ ? std::addressof(*media_) : nullptr; }
const MediaFile* Scene::media() const noexcept { return media_ ? std::addressof(*media_) : nullptr; }

void Scene::setEffect(EffectGraph graph) {
    effect_ = std::move(graph);
    markDirty(DirtyFlag::effect);
}

void Scene::clearEffect() {
    effect_.reset();
    markDirty(DirtyFlag::effect);
}

EffectGraph* Scene::effect() noexcept { return effect_ ? std::addressof(*effect_) : nullptr; }
const EffectGraph* Scene::effect() const noexcept { return effect_ ? std::addressof(*effect_) : nullptr; }

void Scene::markDirty(DirtyFlag flags) noexcept {
    dirty_ |= flags;
    if (flags != DirtyFlag::none) accumulatedSamples_ = 0;
}
DirtyFlag Scene::dirtyFlags() const noexcept { return dirty_; }
bool Scene::dirty(DirtyFlag flags) const noexcept { return (dirty_ & flags) != DirtyFlag::none; }
void Scene::clearDirty(DirtyFlag flags) noexcept {
    dirty_ = static_cast<DirtyFlag>(static_cast<std::uint32_t>(dirty_)
                                  & ~static_cast<std::uint32_t>(flags));
}
void Scene::setRuntimeMode(RuntimeMode mode) noexcept {
    if (runtimeMode_ == mode) return;
    runtimeMode_ = mode;
    markDirty(DirtyFlag::output);
}

void Scene::refreshModelResources(ModelInstance& instance) {
    instance.textures.clear();
    instance.textures.resize(instance.model->textures.size());
    for (std::size_t index = 0; index < instance.model->textures.size(); ++index) {
        try {
            auto texturePath = instance.model->textures[index];
            if (!std::filesystem::exists(texturePath)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         instance.sourcePath.parent_path())) {
                    if (!entry.is_regular_file() || entry.path().extension() != texturePath.extension()) continue;
                    if (entry.path().filename() == texturePath.filename()) {
                        texturePath = entry.path();
                        break;
                    }
                }
            }
            if (std::filesystem::exists(texturePath)) instance.textures[index] = loadImageRgba8(texturePath);
        } catch (const std::exception&) {
            // Missing optional sphere/toon maps use the renderer's fallback texture.
        }
    }
}

} // namespace dayo::core
