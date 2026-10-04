#pragma once
#include <cstdint>

namespace dayo::graphics {
enum class PreviewQuality { automatic, apu, high };
enum class PresentMode { fifo, fifoRelaxed, mailbox, immediate };
struct VulkanOptions {
    PreviewQuality quality{PreviewQuality::automatic};
    PresentMode present{PresentMode::fifo};
    bool asyncCompute{};
};
struct PreviewQualitySettings {
    std::uint32_t shadowSize{2048};
    std::uint32_t aoDivisor{2};
    std::uint32_t environmentFaceSize{512};
    bool cheapShadow{};
};
// Zero budget means unavailable; retain the device-based profile in that case.
constexpr PreviewQualitySettings previewQualitySettings(PreviewQuality quality, bool integrated,
                                                        std::uint64_t available = 0) noexcept {
    PreviewQualitySettings settings;
    if (quality == PreviewQuality::apu || (quality == PreviewQuality::automatic && integrated))
        settings = {1024, 2, 256, true};
    if (quality != PreviewQuality::automatic || available == 0)
        return settings;
    constexpr auto mib = std::uint64_t{1024} * 1024;
    if (available < 512 * mib)
        return {1024, 4, 128, true};
    if (available < 1024 * mib)
        return {1024, 2, 256, true};
    return settings;
}
} // namespace dayo::graphics
