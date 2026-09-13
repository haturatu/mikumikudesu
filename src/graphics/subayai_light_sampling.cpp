#include "graphics/subayai_light_sampling.hpp"

#include "core/log.hpp"

#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dayo::graphics {

void LightSamplingService::update(std::span<const float> lightPowers, bool lightingDirty) {
    if (!lightingDirty) {
        log::debug("Light sampling unchanged; keeping alias table");
        return;
    }
    table_.clear();
    const std::size_t count = lightPowers.size();
    if (count == 0) {
        log::debug("Light sampling cleared: no lights");
        return;
    }
    double sum = 0.0;
    for (const auto power : lightPowers) {
        sum += static_cast<double>(power < 0.0F ? 0.0F : power);
    }
    table_.resize(count);
    if (!(sum > 0.0)) {
        for (std::size_t index = 0; index < count; ++index) {
            table_[index].probability = 1.0F;
            table_[index].alias = static_cast<std::uint32_t>(index);
        }
        ++builds_;
        log::info("Light alias rebuilt: ", count, " uniform lights");
        return;
    }
    std::vector<double> scaled(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double power = static_cast<double>(lightPowers[index] < 0.0F ? 0.0F : lightPowers[index]);
        scaled[index] = power * static_cast<double>(count) / sum;
    }
    std::vector<std::size_t> small;
    std::vector<std::size_t> large;
    small.reserve(count);
    large.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (scaled[index] < 1.0) {
            small.push_back(index);
        } else {
            large.push_back(index);
        }
    }
    std::vector<double> prob(count, 1.0);
    std::vector<std::uint32_t> alias(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        alias[index] = static_cast<std::uint32_t>(index);
    }
    while (!small.empty() && !large.empty()) {
        const std::size_t less = small.back();
        small.pop_back();
        const std::size_t more = large.back();
        large.pop_back();
        prob[less] = scaled[less];
        alias[less] = static_cast<std::uint32_t>(more);
        scaled[more] = scaled[more] + scaled[less] - 1.0;
        if (scaled[more] < 1.0) {
            small.push_back(more);
        } else {
            large.push_back(more);
        }
    }
    for (const auto index : small) {
        prob[index] = 1.0;
    }
    for (const auto index : large) {
        prob[index] = 1.0;
    }
    for (std::size_t index = 0; index < count; ++index) {
        table_[index].probability = static_cast<float>(prob[index]);
        table_[index].alias = alias[index];
    }
    ++builds_;
    log::info("Light alias rebuilt: ", count, " lights");
}

void LightSamplingService::clear() noexcept {
    table_.clear();
}

LightSamplingGpuRuntime::~LightSamplingGpuRuntime() {
    reset();
}

bool LightSamplingGpuRuntime::sync(Device& device, std::span<const AliasEntry> table, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (table.empty()) {
        if (device_ == &device)
            reset();
        else if (device_ != nullptr) {
            if (error != nullptr)
                *error = "light sampling buffer belongs to a different device";
            return false;
        }
        return true;
    }
    if (table.size() > std::numeric_limits<std::size_t>::max() / sizeof(AliasEntry)) {
        if (error != nullptr)
            *error = "light sampling table size overflow";
        return false;
    }
    if (device_ != nullptr && device_ != &device) {
        if (error != nullptr)
            *error = "light sampling buffer belongs to a different device";
        return false;
    }
    try {
        if (device_ == &device && count_ == table.size() && buffer_.valid()) {
            device.uploadBufferEx(buffer_, std::as_bytes(table), 0);
            return true;
        }
        reset();
        device_ = &device;
        buffer_ = device.createBufferEx({
            .size = table.size() * sizeof(AliasEntry),
            .usage = ResourceUsage::storageRead | ResourceUsage::transferDst | ResourceUsage::hostRead,
            .cpuVisible = true,
            .lifetime = ResourceLifetime::persistent,
        });
        device.uploadBufferEx(buffer_, std::as_bytes(table), 0);
        count_ = table.size();
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = std::string("light sampling GPU upload failed: ") + exception.what();
        reset();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "light sampling GPU upload failed";
        reset();
        return false;
    }
    return true;
}

void LightSamplingGpuRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr && buffer_.valid()) {
        try {
            device->waitIdle();
            device->destroyBufferEx(buffer_);
        } catch (...) {
        }
    }
    device_ = nullptr;
    buffer_ = {};
    count_ = 0;
}

} // namespace dayo::graphics
