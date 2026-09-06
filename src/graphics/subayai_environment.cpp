#include "graphics/subayai_environment.hpp"

#include "core/log.hpp"

namespace dayo::graphics {

bool EnvironmentService::update(const EnvironmentDesc& desc) {
    if (ready_ && cached_ == desc) {
        log::debug("Environment unchanged; reusing cubemap/prefiltered/SH/Skywalker");
        return false;
    }
    if (backend_ != nullptr) {
        backend_->regenerate(desc);
    }
    cached_ = desc;
    ready_ = true;
    ++generations_;
    skywalkerVersion_ = desc.version;
    log::info("Environment regenerated: ", desc.source, " exposure ", desc.exposure);
    return true;
}

void EnvironmentService::setHandles(TextureHandle cubemap, TextureHandle prefiltered,
                                    std::uint64_t skywalkerVersion) noexcept {
    cubemap_ = cubemap;
    prefiltered_ = prefiltered;
    skywalkerVersion_ = skywalkerVersion;
}

} // namespace dayo::graphics
