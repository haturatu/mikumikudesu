#include "graphics/native_fx_global_variable_runtime.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

} // namespace

NativeFxGlobalVariableRuntime::~NativeFxGlobalVariableRuntime() {
    reset();
}

bool NativeFxGlobalVariableRuntime::initialize(Device& device, std::uint32_t size, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    if (size > kMaxNativeFxGlobalVariableBytes) {
        setError(error, "YRZFX global variable buffer exceeds the 64 KiB constant-buffer limit");
        return false;
    }
    const auto deviceLimit = device.capabilities().maxUniformBufferRange;
    if (size != 0 && deviceLimit != 0 && size > deviceLimit) {
        setError(error, "YRZFX global variable buffer exceeds this device's uniform-buffer range");
        return false;
    }

    device_ = &device;
    size_ = size;
    if (size_ == 0)
        return true;

    try {
        buffer_ = device.createBufferEx({.size = size_,
                                         .usage = ResourceUsage::uniformRead | ResourceUsage::hostRead,
                                         .cpuVisible = true,
                                         .lifetime = ResourceLifetime::persistent});
        if (!buffer_.valid())
            throw std::runtime_error("YRZFX global variable buffer allocation returned an invalid handle");
        const std::vector<std::byte> zeroes(size_, std::byte{0});
        device.uploadBufferEx(buffer_, zeroes, 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("YRZFX global variable buffer initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "YRZFX global variable buffer initialization failed");
        reset();
        return false;
    }
    return true;
}

void NativeFxGlobalVariableRuntime::reset() noexcept {
    if (device_ != nullptr && buffer_.valid()) {
        try {
            device_->destroyBufferEx(buffer_);
        } catch (...) {
        }
    }
    device_ = nullptr;
    buffer_ = {};
    size_ = 0;
}

} // namespace dayo::graphics
