#pragma once

#include "fx/fx_frame.hpp"
#include "core/fx/fx_controller_resolver.hpp"
#include "graphics/native_controller_runtime.hpp"
#include "graphics/native_frame_constants.hpp"
#include "graphics/native_scene_resource_runtime.hpp"

#include <optional>
#include <span>
#include <string>

namespace dayo::graphics {

// Frame-boundary owner for the complete native scene ABI. It combines the
// fixed descriptor spaces with the three uniform buffers whose contents must
// describe the same frame. Scene/model code still supplies the resource
// handles; this class supplies and validates the ABI-critical constants.
class NativeSceneFrameRuntime {
  public:
    NativeSceneFrameRuntime() = default;
    ~NativeSceneFrameRuntime();

    NativeSceneFrameRuntime(const NativeSceneFrameRuntime&) = delete;
    NativeSceneFrameRuntime& operator=(const NativeSceneFrameRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, std::span<const core::EffectController> controllers,
                                  const NativeSceneDescriptorCounts& counts = {}, std::string* error = nullptr);
    [[nodiscard]] bool syncViewConstants(const fx::FxFrameContext& context, std::string* error = nullptr);
    [[nodiscard]] bool sync(const fx::FxFrameContext& context, NativeSceneResourceBindings resources,
                            const NativeScenePassConstants& pass = {}, std::string* error = nullptr);
    [[nodiscard]] bool updatePassConstants(CommandList& commands, const NativeScenePassConstants& pass,
                                           std::string* error = nullptr);
    [[nodiscard]] bool syncControllers(std::string* error = nullptr);
    [[nodiscard]] bool syncControllers(std::span<const core::EffectController> declarations,
                                       const core::fx::SceneEvaluationSnapshot& snapshot, core::ModelId self,
                                       std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && scene_.ready() && constants_.ready() && controllers_.ready() &&
               controllerBlock_.has_value();
    }
    [[nodiscard]] NativeControllerBlock* controllerBlock() noexcept {
        return controllerBlock_.has_value() ? &*controllerBlock_ : nullptr;
    }
    [[nodiscard]] const NativeControllerBlock* controllerBlock() const noexcept {
        return controllerBlock_.has_value() ? &*controllerBlock_ : nullptr;
    }
    [[nodiscard]] std::span<const handles::DescriptorSetLayoutHandle> layouts() const noexcept {
        return scene_.layouts();
    }
    [[nodiscard]] std::span<const handles::DescriptorSetHandle> descriptorSets() const noexcept {
        return scene_.descriptorSets();
    }
    [[nodiscard]] bool descriptorSetsReady() const noexcept;
    [[nodiscard]] const NativeSceneResourceRuntime& resources() const noexcept {
        return scene_;
    }
    [[nodiscard]] const NativeFrameConstantsRuntime& constants() const noexcept {
        return constants_;
    }
    [[nodiscard]] const NativeControllerRuntime& controllers() const noexcept {
        return controllers_;
    }

  private:
    Device* device_{};
    NativeSceneResourceRuntime scene_;
    NativeFrameConstantsRuntime constants_;
    NativeControllerRuntime controllers_;
    std::optional<NativeControllerBlock> controllerBlock_;
};

} // namespace dayo::graphics
