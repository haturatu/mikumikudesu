#include "fx/fx_runtime_requirements.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <ranges>
#include <sstream>

namespace dayo::fx {
namespace {

[[nodiscard]] bool isDds(std::string_view filename) {
    if (filename.size() < 4)
        return false;
    const auto extension = std::filesystem::path(filename).extension().string();
    std::string lowered;
    lowered.reserve(extension.size());
    for (const auto character : extension)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return lowered == ".dds";
}

} // namespace

bool FxRuntimeRequirements::contains(FxRuntimeFeature feature) const noexcept {
    return std::ranges::find(features, feature) != features.end();
}

void FxRuntimeRequirements::add(FxRuntimeFeature feature) {
    if (!contains(feature))
        features.push_back(feature);
}

const char* toString(FxRuntimeFeature feature) noexcept {
    switch (feature) {
    case FxRuntimeFeature::textures3D:
        return "textures3D";
    case FxRuntimeFeature::externalDds:
        return "externalDds";
    case FxRuntimeFeature::persistentResources:
        return "persistentResources";
    case FxRuntimeFeature::sharedResources:
        return "sharedResources";
    case FxRuntimeFeature::matDesc:
        return "matDesc";
    case FxRuntimeFeature::bufferRaster:
        return "bufferRaster";
    case FxRuntimeFeature::stencil:
        return "stencil";
    case FxRuntimeFeature::logicOp:
        return "logicOp";
    case FxRuntimeFeature::alphaToCoverage:
        return "alphaToCoverage";
    case FxRuntimeFeature::vertexLayout:
        return "vertexLayout";
    case FxRuntimeFeature::colorWriteMask:
        return "colorWriteMask";
    case FxRuntimeFeature::meshCloning:
        return "meshCloning";
    case FxRuntimeFeature::deformHostAbi:
        return "deformHostAbi";
    case FxRuntimeFeature::rayTracing:
        return "rayTracing";
    case FxRuntimeFeature::oidn:
        return "oidn";
    case FxRuntimeFeature::conditions:
        return "conditions";
    case FxRuntimeFeature::globalVariables:
        return "globalVariables";
    case FxRuntimeFeature::fullSamplerState:
        return "fullSamplerState";
    case FxRuntimeFeature::functionalCopy:
        return "functionalCopy";
    case FxRuntimeFeature::functionalClearRtv:
        return "functionalClearRtv";
    case FxRuntimeFeature::functionalClearUav:
        return "functionalClearUav";
    case FxRuntimeFeature::functionalMipmap:
        return "functionalMipmap";
    }
    return "unknown";
}

FxRuntimeRequirements analyzeRuntimeRequirements(const FxProgram& program) {
    FxRuntimeRequirements result;
    if (!program.textures3D.empty())
        result.add(FxRuntimeFeature::textures3D);
    if (!program.materialDescriptor.has_value()) {
        // Keep the test explicit: a material descriptor is a feature request,
        // not a property inferred from the presence of a renderer name.
    } else {
        result.add(FxRuntimeFeature::matDesc);
    }
    if (program.globalVarSize != 0)
        result.add(FxRuntimeFeature::globalVariables);
    if (program.meshCloneCount > 1)
        result.add(FxRuntimeFeature::meshCloning);
    if (program.category == core::fx::FxCategory::deform)
        result.add(FxRuntimeFeature::deformHostAbi);
    for (const auto& texture : program.textures) {
        if (!texture.filename.empty() && isDds(texture.filename))
            result.add(FxRuntimeFeature::externalDds);
        if (!texture.shared.empty())
            result.add(FxRuntimeFeature::sharedResources);
    }
    for (const auto& texture : program.textures3D) {
        if (!texture.filename.empty() && isDds(texture.filename))
            result.add(FxRuntimeFeature::externalDds);
        if (!texture.shared.empty())
            result.add(FxRuntimeFeature::sharedResources);
    }
    for (const auto& buffer : program.buffers)
        if (!buffer.shared.empty())
            result.add(FxRuntimeFeature::sharedResources);
    for (const auto& dispatch : program.passes) {
        if (!dispatch.conditions.empty())
            result.add(FxRuntimeFeature::conditions);
        if (dispatch.kind == FxOpKind::raytracing)
            result.add(FxRuntimeFeature::rayTracing);
        if (dispatch.kind == FxOpKind::oidn)
            result.add(FxRuntimeFeature::oidn);
        if (dispatch.functionalKind == core::EffectFunctionalPassKind::copy)
            result.add(FxRuntimeFeature::functionalCopy);
        else if (dispatch.functionalKind == core::EffectFunctionalPassKind::clearRtv)
            result.add(FxRuntimeFeature::functionalClearRtv);
        else if (dispatch.functionalKind == core::EffectFunctionalPassKind::clearUav)
            result.add(FxRuntimeFeature::functionalClearUav);
        else if (dispatch.functionalKind == core::EffectFunctionalPassKind::mipmapGen)
            result.add(FxRuntimeFeature::functionalMipmap);
        if (const auto* raster = std::get_if<FxRasterDispatch>(&dispatch.executable); raster != nullptr) {
            if (raster->graphics.modelTarget == core::fx::RasterModelTarget::buffer)
                result.add(FxRuntimeFeature::bufferRaster);
            if (raster->graphics.depthStencil.stencilEnable)
                result.add(FxRuntimeFeature::stencil);
            if (raster->graphics.logicOpEnable)
                result.add(FxRuntimeFeature::logicOp);
            if (raster->graphics.alphaToCoverage)
                result.add(FxRuntimeFeature::alphaToCoverage);
            if (!raster->vertexLayout.bindings.empty() || !raster->vertexLayout.attributes.empty())
                result.add(FxRuntimeFeature::vertexLayout);
            if (raster->rasterSource == core::EffectRasterSource::buffer)
                result.add(FxRuntimeFeature::bufferRaster);
            if (!raster->graphics.blend.empty()) {
                for (const auto& target : raster->graphics.blend)
                    if (target.colorWriteMask != 0x0FU)
                        result.add(FxRuntimeFeature::colorWriteMask);
            }
        }
    }
    for (const auto& sampler : program.samplers) {
        if (sampler.filterKind != core::FxFilter::linear || sampler.maxAnisotropy > 1 || sampler.mipLodBias != 0.0F || sampler.minLod != 0.0F ||
            sampler.maxLod != std::numeric_limits<float>::max() || sampler.comparisonFunc != core::FxCompareOp::always ||
            sampler.borderColor != core::FxBorderColor::transparentBlack || sampler.addressW != "WRAP")
            result.add(FxRuntimeFeature::fullSamplerState);
    }
    return result;
}

std::vector<FxRuntimeFeature> missingRuntimeFeatures(const FxRuntimeRequirements& required,
                                                     const FxRuntimeRequirements& implemented) {
    std::vector<FxRuntimeFeature> missing;
    for (const auto feature : required.features)
        if (!implemented.contains(feature))
            missing.push_back(feature);
    return missing;
}

std::string formatRuntimeFeatureList(const FxRuntimeRequirements& requirements) {
    std::ostringstream output;
    for (std::size_t index = 0; index < requirements.features.size(); ++index) {
        if (index != 0)
            output << ',';
        output << toString(requirements.features[index]);
    }
    return output.str();
}

} // namespace dayo::fx
