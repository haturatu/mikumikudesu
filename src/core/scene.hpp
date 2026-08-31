#pragma once

#include "core/effect.hpp"
#include "core/image.hpp"
#include "core/media.hpp"
#include "core/animation.hpp"
#include "core/motion.hpp"
#include "core/physics.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dayo::core {

using ModelId = std::uint64_t;

enum class ScreenTextureSource { previousFrame, backgroundVideo, backgroundImage, white };
enum class ScreenCropMode { none, crop4x3 };
enum class BackgroundMode { opaque, alpha };
enum class RuntimeMode { accumulate, realtime, idle };

enum class DirtyFlag : std::uint32_t {
    none = 0,
    camera = 1U << 0U,
    geometry = 1U << 1U,
    material = 1U << 2U,
    lighting = 1U << 3U,
    effect = 1U << 4U,
    output = 1U << 5U,
    background = 1U << 6U,
};

[[nodiscard]] constexpr DirtyFlag operator|(DirtyFlag left, DirtyFlag right) noexcept {
    return static_cast<DirtyFlag>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr DirtyFlag operator&(DirtyFlag left, DirtyFlag right) noexcept {
    return static_cast<DirtyFlag>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}
constexpr DirtyFlag& operator|=(DirtyFlag& left, DirtyFlag right) noexcept { return left = left | right; }

struct PhysicsSettings {
    float gravity { 98.0F };
    Float3 gravityDirection { 0.0F, -1.0F, 0.0F };
    float noiseAmplitude {};
    float noiseFrequency {};
    bool floorCollision {};
};

struct ExternalParentLink {
    ModelId parentModel {};
    std::string parentBone;
    ModelId childModel {};
    std::string childBone;
};

struct BackgroundState {
    std::optional<ImageRgba8> image;
    std::optional<std::filesystem::path> imagePath;
    std::optional<std::filesystem::path> videoPath;
    ScreenTextureSource screenSource { ScreenTextureSource::white };
    ScreenCropMode crop { ScreenCropMode::none };
    BackgroundMode mode { BackgroundMode::opaque };
    bool enabled { true };
};

struct ModelInstance {
    ModelId id {};
    std::filesystem::path sourcePath;
    std::shared_ptr<PmxModel> model;
    std::unique_ptr<MmdAnimator> animator;
    std::unique_ptr<MmdPhysics> physics;
    std::unique_ptr<SoftBodySimulation> softBody;
    std::unique_ptr<VmdMotion> motion;
    std::unique_ptr<VpdPose> pose;
    std::vector<ImageRgba8> textures;
    bool visible { true };
    std::uint32_t cloneCount { 1 };
    PreviewNormalization normalization;
    std::string displayName;
};

struct Timeline {
    float frame {};
    float fps { 30.0F };
    float duration {};
    std::vector<VmdCameraKey> camera;
    std::vector<VmdLightKey> light;
    std::vector<VmdShadowKey> shadow;
    std::vector<VmdIkKey> ik;
    std::vector<ExternalParentLink> externalParents;
    std::vector<std::pair<std::uint32_t, PhysicsSettings>> gravity;
};

class Scene {
public:
    Scene();
    ~Scene();
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] ModelId addModel(const std::filesystem::path& path);
    bool removeModel(ModelId id);
    void clearModels();
    [[nodiscard]] ModelInstance* model(ModelId id) noexcept;
    [[nodiscard]] const ModelInstance* model(ModelId id) const noexcept;
    [[nodiscard]] ModelInstance* selectedModel() noexcept;
    [[nodiscard]] const ModelInstance* selectedModel() const noexcept;
    void selectModel(ModelId id) noexcept;

    // Clears every asset and timeline-owned value before loading another
    // project. This is intentionally separate from clearModels(), which may
    // be used while keeping a global camera/light motion in the scene.
    void clearProjectState();
    void attachMotion(const std::filesystem::path& path, ModelId target = 0);
    void attachMotion(VmdMotion motion, ModelId target = 0);
    void attachMotion(MotionDocument document, ModelId target = 0, std::string modelName = {});
    void attachPose(const std::filesystem::path& path, ModelId target = 0);
    [[nodiscard]] ModelId targetModel(ModelId requested = 0) const noexcept;
    bool addExternalParent(ExternalParentLink link, std::string* error = nullptr);
    [[nodiscard]] bool hasExternalParentCycle() const noexcept;
    [[nodiscard]] PhysicsSettings evaluatePhysicsSettings(float frame) const noexcept;
    void recalculateTimelineDuration() noexcept;

    void setFrame(float frame) noexcept;
    void setTimelineDuration(float duration) noexcept;
    void setGravityTrack(std::vector<std::pair<std::uint32_t, PhysicsSettings>> track);
    // Advances the one Scene timeline shared by every model and camera track.
    // Realtime playback is the only automatic timeline driver; accumulate
    // keeps the current frame fixed while sampling and idle waits for edits.
    // Returns true when the frame changed.
    bool advanceFrame(float deltaSeconds, bool playing) noexcept;
    void setPhysicsSettings(PhysicsSettings settings) noexcept;
    bool setModelVisible(ModelId id, bool visible) noexcept;
    bool setCloneCount(ModelId id, std::uint32_t cloneCount) noexcept;
    void setBackgroundScreenSource(ScreenTextureSource source) noexcept;
    void setBackgroundEnabled(bool enabled) noexcept;

    void setBackgroundImage(const std::filesystem::path& path);
    void setBackgroundVideo(const std::filesystem::path& path);
    void setMedia(const std::filesystem::path& path);
    void clearMedia();
    void clearBackground();
    [[nodiscard]] MediaFile* media() noexcept;
    [[nodiscard]] const MediaFile* media() const noexcept;

    void setEffect(EffectGraph graph);
    void clearEffect();
    [[nodiscard]] EffectGraph* effect() noexcept;
    [[nodiscard]] const EffectGraph* effect() const noexcept;

    void markDirty(DirtyFlag flags) noexcept;
    [[nodiscard]] DirtyFlag dirtyFlags() const noexcept;
    [[nodiscard]] bool dirty(DirtyFlag flags) const noexcept;
    [[nodiscard]] std::uint64_t accumulatedSamples() const noexcept { return accumulatedSamples_; }
    void advanceAccumulation() noexcept { ++accumulatedSamples_; }
    void invalidateAccumulation() noexcept { accumulatedSamples_ = 0; }
    void clearDirty(DirtyFlag flags = DirtyFlag::camera | DirtyFlag::geometry | DirtyFlag::material
                                  | DirtyFlag::lighting | DirtyFlag::effect | DirtyFlag::output
                                  | DirtyFlag::background) noexcept;
    void setRuntimeMode(RuntimeMode mode) noexcept;

    [[nodiscard]] const std::vector<ModelInstance>& models() const noexcept { return models_; }
    [[nodiscard]] const BackgroundState& background() const noexcept { return background_; }
    [[nodiscard]] const Timeline& timeline() const noexcept { return timeline_; }
    [[nodiscard]] const PhysicsSettings& physicsSettings() const noexcept { return physicsSettings_; }
    [[nodiscard]] const std::vector<ExternalParentLink>& externalParents() const noexcept { return externalParents_; }
    [[nodiscard]] const VmdMotion* cameraMotion() const noexcept { return cameraMotion_.get(); }
    [[nodiscard]] RuntimeMode runtimeMode() const noexcept { return runtimeMode_; }
    [[nodiscard]] ModelId selectedModelId() const noexcept { return selectedModel_; }
    [[nodiscard]] std::uint64_t topologyGeneration() const noexcept { return topologyGeneration_; }

private:
    void refreshModelResources(ModelInstance& instance);
    ModelId nextId_ { 1 };
    ModelId selectedModel_ {};
    std::vector<ModelInstance> models_;
    std::unique_ptr<VmdMotion> cameraMotion_;
    BackgroundState background_;
    Timeline timeline_;
    PhysicsSettings physicsSettings_;
    std::vector<ExternalParentLink> externalParents_;
    std::optional<MediaFile> media_;
    std::optional<EffectGraph> effect_;
    DirtyFlag dirty_ { DirtyFlag::camera | DirtyFlag::geometry | DirtyFlag::material | DirtyFlag::lighting
                     | DirtyFlag::background };
    RuntimeMode runtimeMode_ { RuntimeMode::realtime };
    std::uint64_t accumulatedSamples_ {};
    std::uint64_t topologyGeneration_ { 1 };
};

} // namespace dayo::core
