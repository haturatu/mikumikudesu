#include "graphics/sbt.hpp"

#include "core/log.hpp"

#include <limits>
#include <optional>

namespace dayo::graphics {

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    const auto remainder = value % alignment;
    if (remainder == 0)
        return value;
    const auto delta = alignment - remainder;
    if (value > std::numeric_limits<std::uint32_t>::max() - delta)
        return 0;
    return value + delta;
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

ShaderBindingTableBuilder::Layout ShaderBindingTableBuilder::build(std::uint64_t baseAddress,
                                                                   const Properties& properties) const noexcept {
    Layout layout;
    if (raygen_.empty()) {
        log::debug("SBT build skipped: no raygen group");
        return layout;
    }
    const std::uint32_t handleAlignment = properties.handleAlignment == 0 ? 1U : properties.handleAlignment;
    const std::uint32_t baseAlignment = properties.baseAlignment == 0 ? 1U : properties.baseAlignment;
    const std::uint32_t stride = alignUp(properties.handleSize == 0 ? 1U : properties.handleSize, handleAlignment);
    if (stride == 0 || stride > properties.maxStride) {
        log::error("SBT build rejected stride outside Vulkan limits");
        return layout;
    }
    layout.raygenStride = stride;
    layout.missStride = stride;
    layout.hitStride = stride;
    const auto alignAddress = [baseAlignment](std::uint64_t address) -> std::optional<std::uint64_t> {
        const auto alignment = static_cast<std::uint64_t>(baseAlignment);
        const auto remainder = address % alignment;
        if (remainder == 0)
            return address;
        const auto delta = alignment - remainder;
        if (address > std::numeric_limits<std::uint64_t>::max() - delta)
            return std::nullopt;
        return address + delta;
    };
    const auto raygenAddress = alignAddress(baseAddress);
    if (!raygenAddress.has_value())
        return Layout{};
    layout.raygenAddress = *raygenAddress;
    const auto raygenSize = static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(raygen_.size());
    const auto missSize = static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(miss_.size());
    const auto hitSize = static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(hitGroups_.size());
    if (layout.raygenAddress > std::numeric_limits<std::uint64_t>::max() - raygenSize)
        return Layout{};
    const auto missAddress = alignAddress(layout.raygenAddress + raygenSize);
    if (!missAddress.has_value())
        return Layout{};
    layout.missAddress = *missAddress;
    if (layout.missAddress > std::numeric_limits<std::uint64_t>::max() - missSize)
        return Layout{};
    const auto hitAddress = alignAddress(layout.missAddress + missSize);
    if (!hitAddress.has_value())
        return Layout{};
    layout.hitAddress = *hitAddress;
    if (layout.hitAddress > std::numeric_limits<std::uint64_t>::max() - hitSize)
        return Layout{};
    const auto endAddress = layout.hitAddress + hitSize;
    if (endAddress < layout.hitAddress || endAddress < baseAddress)
        return Layout{};
    layout.totalSize = endAddress - baseAddress;
    return layout;
}

} // namespace dayo::graphics
