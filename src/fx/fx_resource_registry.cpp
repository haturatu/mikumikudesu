#include "fx/fx_resource_registry.hpp"

namespace dayo::fx {

void FxResourceRegistry::registerProgram(const std::string& name, std::shared_ptr<const FxProgram> program) {
    std::scoped_lock lock(mutex_);
    programs_[name] = std::move(program);
}

std::shared_ptr<const FxProgram> FxResourceRegistry::findProgram(const std::string& name) const {
    std::scoped_lock lock(mutex_);
    const auto found = programs_.find(name);
    return found == programs_.end() ? nullptr : found->second;
}

void FxResourceRegistry::unregisterProgram(const std::string& name) {
    std::scoped_lock lock(mutex_);
    programs_.erase(name);
}

void FxResourceRegistry::storeFrameContext(FxFrameContext context) {
    std::scoped_lock lock(mutex_);
    frameContext_ = context;
}

FxFrameContext FxResourceRegistry::loadFrameContext() const {
    std::scoped_lock lock(mutex_);
    return frameContext_;
}

void FxResourceRegistry::clear() noexcept {
    std::scoped_lock lock(mutex_);
    programs_.clear();
    frameContext_ = {};
}

std::size_t FxResourceRegistry::programCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return programs_.size();
}

} // namespace dayo::fx
