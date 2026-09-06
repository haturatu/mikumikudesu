#include "graphics/sbt.hpp"

#include "core/log.hpp"

namespace dayo::graphics {

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    const std::uint32_t mask = alignment - 1U;
    return (value + mask) & ~mask;
}

void ShaderBindingTableBuilder::setRaygen(std::string name) {
    raygen_.clear();
    raygen_.push_back(std::move(name));
    log::debug("SBT raygen set");
}

void ShaderBindingTableBuilder::addMiss(std::string name) {
    miss_.push_back(std::move(name));
    log::debug("SBT miss added: ", miss_.size());
}

void ShaderBindingTableBuilder::addHitGroup(std::string name) {
    hitGroups_.push_back(std::move(name));
    log::debug("SBT hit group added: ", hitGroups_.size());
}

void ShaderBindingTableBuilder::clear() noexcept {
    raygen_.clear();
    miss_.clear();
    hitGroups_.clear();
}

ShaderBindingTableBuilder::Layout ShaderBindingTableBuilder::build(std::uint64_t baseAddress, std::uint32_t handleSize,
                                                                   std::uint32_t handleAlignment) const noexcept {
    Layout layout;
    if (raygen_.empty()) {
        log::debug("SBT build skipped: no raygen group");
        return layout;
    }
    const std::uint32_t alignment = handleAlignment == 0 ? 1U : handleAlignment;
    const std::uint32_t stride = alignUp(handleSize == 0 ? 1U : handleSize, alignment);
    layout.raygenStride = stride;
    layout.missStride = stride;
    layout.hitStride = stride;
    layout.raygenAddress = baseAddress;
    const auto raygenSize = static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(raygen_.size());
    const auto missSize = static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(miss_.size());
    const auto hitSize = static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(hitGroups_.size());
    layout.missAddress = baseAddress + raygenSize;
    layout.hitAddress = layout.missAddress + missSize;
    layout.totalSize = raygenSize + missSize + hitSize;
    return layout;
}

} // namespace dayo::graphics
