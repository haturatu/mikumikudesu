#include "graphics/device.hpp"

#include <vulkan/vulkan_core.h>

#include <sstream>

namespace dayo::graphics {

bool DeviceCapabilities::supports(RendererKind renderer) const noexcept {
    switch (renderer) {
    case RendererKind::preview: return supportsPreview();
    case RendererKind::subayai: return supportsSubayai();
    case RendererKind::bdpt: return supportsBdpt();
    }
    return false;
}

std::string DeviceCapabilities::missingFeatures(RendererKind renderer) const {
    std::ostringstream output;
    auto add = [&output](std::string_view feature) {
        if (output.tellp() > 0) output << ", ";
        output << feature;
    };
    if (!swapchain) add("VK_KHR_swapchain");
    if (renderer != RendererKind::preview) {
        if (!bufferDeviceAddress) add("bufferDeviceAddress");
        if (!descriptorIndexing) add("descriptorIndexing");
        if (!accelerationStructure) add("VK_KHR_acceleration_structure");
        if (!rayQuery) add("VK_KHR_ray_query");
        if (!fragmentShaderBarycentric) add("VK_KHR_fragment_shader_barycentric");
    }
    if (renderer == RendererKind::bdpt && !rayTracingPipeline) {
        add("VK_KHR_ray_tracing_pipeline");
    }
    return output.str();
}

std::string DeviceCapabilities::json() const {
    std::ostringstream output;
    output << "{\n"
           << "  \"gpu\": \"" << gpuName << "\",\n"
           << "  \"driver\": \"" << driverName << "\",\n"
           << "  \"vendor_id\": " << vendorId << ",\n"
           << "  \"vulkan_api\": \""
           << VK_VERSION_MAJOR(apiVersion) << '.' << VK_VERSION_MINOR(apiVersion) << '.'
           << VK_VERSION_PATCH(apiVersion) << "\",\n"
           << std::boolalpha
           << "  \"preview\": " << supportsPreview() << ",\n"
           << "  \"subayai\": " << supportsSubayai() << ",\n"
           << "  \"bdpt\": " << supportsBdpt() << ",\n"
           << "  \"buffer_device_address\": " << bufferDeviceAddress << ",\n"
           << "  \"descriptor_indexing\": " << descriptorIndexing << ",\n"
           << "  \"acceleration_structure\": " << accelerationStructure << ",\n"
           << "  \"ray_tracing_pipeline\": " << rayTracingPipeline << ",\n"
           << "  \"ray_query\": " << rayQuery << ",\n"
           << "  \"fragment_shader_barycentric\": " << fragmentShaderBarycentric << "\n"
           << '}';
    return output.str();
}

std::string_view toString(RendererKind renderer) noexcept {
    switch (renderer) {
    case RendererKind::preview: return "Preview";
    case RendererKind::subayai: return "Subayai";
    case RendererKind::bdpt: return "BDPT";
    }
    return "Preview";
}

} // namespace dayo::graphics
