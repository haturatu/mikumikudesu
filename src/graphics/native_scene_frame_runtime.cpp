#include "graphics/native_scene_frame_runtime.hpp"

#include "graphics/dayo_host_resources.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

} // namespace

NativeSceneFrameRuntime::~NativeSceneFrameRuntime() {
    reset();
}

bool NativeSceneFrameRuntime::descriptorSetsReady() const noexcept {
    if (!ready())
        return false;
    return std::all_of(scene_.descriptorSets().begin(), scene_.descriptorSets().end(),
                       [](const auto set) { return set.valid(); });
}

bool NativeSceneFrameRuntime::initialize(Device& device, std::span<const core::EffectController> controllers,
                                         const NativeSceneDescriptorCounts& counts, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        if (!scene_.initialize(device, counts, error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                         : "native scene descriptors unavailable");
        if (!constants_.initialize(device, error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                         : "native frame constants unavailable");
        if (!controllers_.initialize(device, controllers, error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                         : "native controller constants unavailable");
        controllerBlock_.emplace(controllers_.layout());
        if (!syncControllers(error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error : "native controller upload failed");
    } catch (const std::exception& exception) {
        if (error == nullptr || error->empty())
            setError(error, std::string("native scene frame initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native scene frame initialization failed");
        reset();
        return false;
    }
    return true;
}

bool NativeSceneFrameRuntime::syncViewConstants(const fx::FxFrameContext& context, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready()) {
        setError(error, "native scene frame runtime is not initialized");
        return false;
    }
    const auto view = makeNativeViewConstants(context, static_cast<std::uint32_t>(context.totalMaterial));
    return constants_.syncView(*device_, view, error);
}

bool NativeSceneFrameRuntime::sync(const fx::FxFrameContext& context, NativeSceneResourceBindings resources,
                                   const NativeScenePassConstants& pass, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready()) {
        setError(error, "native scene frame runtime is not initialized");
        return false;
    }
    if (!syncControllers(error))
        return false;
    const auto view = makeNativeViewConstants(context, static_cast<std::uint32_t>(context.totalMaterial));
    if (!constants_.sync(*device_, view, pass, error))
        return false;
    resources.viewConstants = constants_.viewBuffer();
    resources.controllerConstants = controllers_.buffer();
    resources.passConstants = constants_.passBuffer();
    resources.hostResourceMask |= dayoSemanticBit(DayoSemantic::ViewCB) | dayoSemanticBit(DayoSemantic::ControllerCB) |
                                  dayoSemanticBit(DayoSemantic::CBuff1);
    return scene_.sync(resources, error);
}

bool NativeSceneFrameRuntime::updatePassConstants(CommandList& commands, const NativeScenePassConstants& pass,
                                                  std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready()) {
        setError(error, "native scene frame runtime is not initialized");
        return false;
    }
    try {
        const auto buffer = constants_.passBuffer();
        commands.uploadBufferEx(buffer, std::as_bytes(std::span<const NativeScenePassConstants>(&pass, 1)), 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("native CBuff1 command-list upload failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native CBuff1 command-list upload failed");
        return false;
    }
    return true;
}

bool NativeSceneFrameRuntime::syncControllers(std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr || !controllerBlock_.has_value()) {
        setError(error, "native controller constants are not initialized");
        return false;
    }
    return controllers_.sync(*device_, controllerBlock_->bytes(), error);
}

void NativeSceneFrameRuntime::reset() noexcept {
    // Descriptor sets are released before their dependent uniform buffers.
    scene_.reset();
    constants_.reset();
    controllers_.reset();
    controllerBlock_.reset();
    device_ = nullptr;
}

} // namespace dayo::graphics
