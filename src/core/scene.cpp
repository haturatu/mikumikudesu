#include "core/scene.hpp"

#include <algorithm>
#include <cmath>
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
    instance.animator->setPhysics(instance.physics.get());
    instance.softBody = std::make_unique<SoftBodySimulation>(*instance.model);
    instance.normalization = previewNormalization(*instance.model);
    instance.displayName = instance.model->metadata.modelName.empty()
        ? path.filename().string() : instance.model->metadata.modelName;
    const auto order = static_cast<std::int32_t>(models_.size());
    instance.order = { order, order, order, order };
    instance.materialSettings.resize(instance.model->materials.size());
    refreshModelResources(instance);
    models_.push_back(std::move(instance));
    ++topologyGeneration_;
    selectedModel_ = models_.back().id;
    markDirty(DirtyFlag::geometry | DirtyFlag::material);
    return selectedModel_;
}

bool Scene::removeModel(ModelId id) {
    const auto found = std::find_if(models_.begin(), models_.end(), [id](const auto& item) { return item.id == id; });
    if (found == models_.end()) return false;
    models_.erase(found);
    ++topologyGeneration_;
    if (selectedModel_ == id) selectedModel_ = models_.empty() ? 0 : models_.front().id;
    externalParents_.erase(std::remove_if(externalParents_.begin(), externalParents_.end(),
        [id](const auto& link) { return link.parentModel == id || link.childModel == id; }), externalParents_.end());
    timeline_.externalParents = externalParents_;
    recalculateTimelineDuration();
    markDirty(DirtyFlag::geometry | DirtyFlag::material);
    return true;
}

void Scene::clearModels() {
    if (!models_.empty()) ++topologyGeneration_;
    models_.clear();
    selectedModel_ = 0;
    externalParents_.clear();
    timeline_.externalParents.clear();
    recalculateTimelineDuration();
    markDirty(DirtyFlag::geometry | DirtyFlag::material);
}

void Scene::clearProjectState() {
    ++topologyGeneration_;
    models_.clear();
    selectedModel_ = 0;
    nextId_ = 1;
    cameraMotion_.reset();
    externalParents_.clear();
    background_ = {};
    media_.reset();
    effect_.reset();
    timeline_ = {};
    timeline_.fps = 30.0F;
    physicsSettings_ = {};
    runtimeMode_ = RuntimeMode::realtime;
    accumulatedSamples_ = 0;
    dirty_ = DirtyFlag::camera | DirtyFlag::geometry | DirtyFlag::material
           | DirtyFlag::lighting | DirtyFlag::effect | DirtyFlag::output
           | DirtyFlag::background;
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

PhysicsSettings Scene::evaluatePhysicsSettings(float frame) const noexcept {
    if (timeline_.gravity.empty()) return physicsSettings_;
    const auto found = std::upper_bound(timeline_.gravity.begin(), timeline_.gravity.end(), frame,
        [](float value, const auto& key) { return value < static_cast<float>(key.first); });
    if (found == timeline_.gravity.begin()) return found->second;
    return std::prev(found)->second;
}

void Scene::recalculateTimelineDuration() noexcept {
    std::uint32_t lastFrame = 0;
    const auto update = [&lastFrame](const VmdMotion* motion) {
        if (motion != nullptr) lastFrame = std::max(lastFrame, motion->lastFrame);
    };
    update(cameraMotion_.get());
    for (const auto& instance : models_) update(instance.motion.get());
    for (const auto& key : timeline_.camera) lastFrame = std::max(lastFrame, key.frame);
    for (const auto& key : timeline_.light) lastFrame = std::max(lastFrame, key.frame);
    for (const auto& key : timeline_.shadow) lastFrame = std::max(lastFrame, key.frame);
    for (const auto& key : timeline_.ik) lastFrame = std::max(lastFrame, key.frame);
    for (const auto& key : timeline_.gravity) lastFrame = std::max(lastFrame, key.first);
    timeline_.duration = static_cast<float>(lastFrame);
    if (timeline_.duration <= 0.0F || !std::isfinite(timeline_.frame)) {
        timeline_.frame = 0.0F;
    } else {
        timeline_.frame = std::clamp(timeline_.frame, 0.0F, timeline_.duration);
    }
}

void Scene::setFrame(float frame) noexcept {
    if (!std::isfinite(frame)) return;
    if (timeline_.frame == frame) return;
    timeline_.frame = std::max(frame, 0.0F);
    markDirty(DirtyFlag::camera | DirtyFlag::geometry | DirtyFlag::lighting);
}

void Scene::setTimelineDuration(float duration) noexcept {
    if (!std::isfinite(duration)) return;
    const auto value = std::max(duration, 0.0F);
    if (timeline_.duration == value) return;
    timeline_.duration = value;
    markDirty(DirtyFlag::output);
}

void Scene::setGravityTrack(std::vector<std::pair<std::uint32_t, PhysicsSettings>> track) {
    std::ranges::sort(track, [](const auto& left, const auto& right) { return left.first < right.first; });
    timeline_.gravity = std::move(track);
    recalculateTimelineDuration();
    markDirty(DirtyFlag::geometry | DirtyFlag::lighting);
}

bool Scene::advanceFrame(float deltaSeconds, bool playing) noexcept {
    if (!playing || runtimeMode_ != RuntimeMode::realtime || timeline_.duration <= 0.0F
        || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0F) return false;
    const float fps = timeline_.fps > 0.0F && std::isfinite(timeline_.fps) ? timeline_.fps : 30.0F;
    const float period = timeline_.duration + 1.0F;
    const float next = std::fmod(std::max(timeline_.frame, 0.0F) + deltaSeconds * fps, period);
    if (next == timeline_.frame) return false;
    setFrame(next);
    return true;
}

void Scene::setPhysicsSettings(PhysicsSettings settings) noexcept {
    if (!std::ranges::all_of(settings.gravityDirection, [](float value) { return std::isfinite(value); })) return;
    if (!std::isfinite(settings.gravity) || !std::isfinite(settings.noiseAmplitude)
        || !std::isfinite(settings.noiseFrequency)) return;
    settings.gravity = std::max(settings.gravity, 0.0F);
    settings.noiseAmplitude = std::max(settings.noiseAmplitude, 0.0F);
    settings.noiseFrequency = std::max(settings.noiseFrequency, 0.0F);
    physicsSettings_ = settings;
    markDirty(DirtyFlag::geometry | DirtyFlag::lighting);
}

bool Scene::setModelVisible(ModelId id, bool visible) noexcept {
    auto* instance = model(id);
    if (instance == nullptr || instance->visible == visible) return false;
    instance->visible = visible;
    ++topologyGeneration_;
    markDirty(DirtyFlag::geometry);
    return true;
}

bool Scene::setCloneCount(ModelId id, std::uint32_t cloneCount) noexcept {
    auto* instance = model(id);
    if (instance == nullptr) return false;
    const auto value = std::clamp(cloneCount, 1U, 1024U);
    if (instance->cloneCount == value) return false;
    instance->cloneCount = value;
    ++topologyGeneration_;
    markDirty(DirtyFlag::geometry);
    return true;
}

void Scene::setBackgroundScreenSource(ScreenTextureSource source) noexcept {
    if (background_.screenSource == source) return;
    background_.screenSource = source;
    markDirty(DirtyFlag::background);
}

void Scene::setBackgroundEnabled(bool enabled) noexcept {
    if (background_.enabled == enabled) return;
    background_.enabled = enabled;
    markDirty(DirtyFlag::background);
}

void Scene::setBackgroundCrop(ScreenCropMode crop) noexcept {
    if (background_.crop == crop) return;
    background_.crop = crop;
    markDirty(DirtyFlag::background);
}

void Scene::setBackgroundMode(BackgroundMode mode) noexcept {
    if (background_.mode == mode) return;
    background_.mode = mode;
    markDirty(DirtyFlag::background | DirtyFlag::output);
}

void Scene::attachMotion(const std::filesystem::path& path, ModelId target) {
    attachMotion(loadVmd(path), target);
}

void Scene::attachMotion(VmdMotion motion, ModelId target) {
    auto modelName = motion.modelName;
    auto document = toMotionDocument(motion);
    attachMotion(std::move(document), target, std::move(modelName));
}

void Scene::attachMotion(MotionDocument document, ModelId target, std::string modelName) {
    auto motion = std::make_unique<VmdMotion>(toVmdMotion(std::move(document), std::move(modelName)));
    // A camera/light-only VMD is global even when a model is selected. Model
    // motion remains the default for files containing bone or morph tracks.
    const bool cameraOnly = motion->bones.empty() && motion->morphs.empty()
        && (!motion->cameras.empty() || !motion->lights.empty() || !motion->shadows.empty());
    auto* destination = cameraOnly ? nullptr : model(targetModel(target));
    if (destination != nullptr) {
        destination->motion = std::move(motion);
        destination->animator->setMotion(destination->motion.get());
        recalculateTimelineDuration();
    } else {
        // Camera/light VMDs are still useful without a model.
        cameraMotion_ = std::move(motion);
        timeline_.camera = cameraMotion_->cameras;
        timeline_.light = cameraMotion_->lights;
        timeline_.shadow = cameraMotion_->shadows;
        timeline_.ik = cameraMotion_->ik;
        recalculateTimelineDuration();
    }
    markDirty(DirtyFlag::geometry | DirtyFlag::camera | DirtyFlag::lighting);
}

const VmdMotion* Scene::motion(ModelId target, bool global) const noexcept {
    if (global) return cameraMotion_.get();
    const auto* instance = model(targetModel(target));
    return instance == nullptr ? nullptr : instance->motion.get();
}

bool Scene::replaceMotion(VmdMotion motionValue, ModelId target, bool global) {
    if (global) {
        cameraMotion_ = std::make_unique<VmdMotion>(std::move(motionValue));
        timeline_.camera = cameraMotion_->cameras;
        timeline_.light = cameraMotion_->lights;
        timeline_.shadow = cameraMotion_->shadows;
        timeline_.ik = cameraMotion_->ik;
    } else {
        auto* instance = model(targetModel(target));
        if (instance == nullptr) return false;
        instance->motion = std::make_unique<VmdMotion>(std::move(motionValue));
        instance->animator->setMotion(instance->motion.get());
    }
    recalculateTimelineDuration();
    markDirty(DirtyFlag::geometry | DirtyFlag::camera | DirtyFlag::lighting);
    return true;
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
