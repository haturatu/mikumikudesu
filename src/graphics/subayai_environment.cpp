#include "graphics/subayai_environment.hpp"

#include "core/log.hpp"

namespace dayo::graphics {

bool EnvironmentService::update(const EnvironmentDesc& desc) {
    if (ready_ && cached_ == desc) {
        log::debug("Environment unchanged; reusing cubemap/prefiltered/SH/Skywalker");
        return false;
    }
    const auto result = backend_ == nullptr ? EnvironmentGpuResult{} : backend_->regenerateEx(desc);
    cached_ = desc;
    ready_ = true;
    ++generations_;
    skywalkerVersion_ = desc.version;
    typedResult_ = result;
    if (typedResult_.skywalkerVersion == 0)
        typedResult_.skywalkerVersion = desc.version;
    sphericalHarmonics_ = typedResult_.sphericalHarmonics;
    log::info("Environment regenerated: ", desc.source, " exposure ", desc.exposure);
    return true;
}

void EnvironmentService::setHandles(TextureHandle cubemap, TextureHandle prefiltered,
                                    std::uint64_t skywalkerVersion) noexcept {
    cubemap_ = cubemap;
    prefiltered_ = prefiltered;
    skywalkerVersion_ = skywalkerVersion;
    typedResult_.cubemap = {};
    typedResult_.prefiltered = {};
    typedResult_.sphericalHarmonics = sphericalHarmonics_;
    typedResult_.skywalkerVersion = skywalkerVersion;
}

void EnvironmentService::setGpuResult(EnvironmentGpuResult result) noexcept {
    cubemap_ = {};
    prefiltered_ = {};
    typedResult_ = result;
    sphericalHarmonics_ = result.sphericalHarmonics;
    skywalkerVersion_ = result.skywalkerVersion;
}

} // namespace dayo::graphics
