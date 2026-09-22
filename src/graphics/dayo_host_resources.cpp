#include "graphics/dayo_host_resources.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

} // namespace

std::string_view toString(DayoSemantic semantic) noexcept {
    switch (semantic) {
    case DayoSemantic::RTOutput:
        return "RTOutput";
    case DayoSemantic::OIDNBuf:
        return "OIDNBuf";
    case DayoSemantic::NormalDepth:
        return "NormalDepth";
    case DayoSemantic::GBuffer1:
        return "GBuffer1";
    case DayoSemantic::GBuffer2:
        return "GBuffer2";
    case DayoSemantic::TLAS:
        return "TLAS";
    case DayoSemantic::Model2Mat:
        return "Model2Mat";
    case DayoSemantic::Mat2Model:
        return "Mat2Model";
    case DayoSemantic::Peekaboo:
        return "Peekaboo";
    case DayoSemantic::MatSelected:
        return "MatSelected";
    case DayoSemantic::Skybox:
        return "Skybox";
    case DayoSemantic::Skywalker:
        return "Skywalker";
    case DayoSemantic::SkywalkerRow:
        return "SkywalkerRow";
    case DayoSemantic::SkyboxSH:
        return "SkyboxSH";
    case DayoSemantic::ScreenBMP:
        return "ScreenBMP";
    case DayoSemantic::CloneCount:
        return "CloneCount";
    case DayoSemantic::ScreenTexture:
        return "ScreenTexture";
    case DayoSemantic::ViewCB:
        return "ViewCB";
    case DayoSemantic::ControllerCB:
        return "ControllerCB";
    case DayoSemantic::TextureTable:
        return "TextureTable";
    case DayoSemantic::CBuff1:
        return "CBuff1";
    }
    return "unknown";
}

std::optional<DayoSemantic> dayoSemanticFromString(std::string_view name) noexcept {
    constexpr std::array semantics{
        DayoSemantic::RTOutput,      DayoSemantic::OIDNBuf,     DayoSemantic::NormalDepth,  DayoSemantic::GBuffer1,
        DayoSemantic::GBuffer2,      DayoSemantic::TLAS,        DayoSemantic::Model2Mat,    DayoSemantic::Mat2Model,
        DayoSemantic::Peekaboo,      DayoSemantic::MatSelected, DayoSemantic::Skybox,       DayoSemantic::Skywalker,
        DayoSemantic::SkywalkerRow,  DayoSemantic::SkyboxSH,    DayoSemantic::ScreenBMP,    DayoSemantic::CloneCount,
        DayoSemantic::ScreenTexture, DayoSemantic::ViewCB,      DayoSemantic::ControllerCB, DayoSemantic::TextureTable,
        DayoSemantic::CBuff1,
    };
    for (const auto semantic : semantics) {
        if (name == toString(semantic))
            return semantic;
    }
    if (name == "YRZFX_ControllerCB")
        return DayoSemantic::ControllerCB;
    return std::nullopt;
}

std::optional<DayoResourceBinding> DayoHostResourceProvider::resolve(DayoSemantic semantic) const noexcept {
    if (bindings_ == nullptr || (bindings_->hostResourceMask & dayoSemanticBit(semantic)) == 0)
        return std::nullopt;
    DayoResourceBinding result;
    switch (semantic) {
    case DayoSemantic::RTOutput:
        result.texture = bindings_->rtOutput;
        break;
    case DayoSemantic::OIDNBuf:
        result.buffer = bindings_->oidnBuffer;
        break;
    case DayoSemantic::NormalDepth:
        result.texture = bindings_->normalDepth;
        break;
    case DayoSemantic::GBuffer1:
        result.texture = bindings_->gbuffer1;
        break;
    case DayoSemantic::GBuffer2:
        result.texture = bindings_->gbuffer2;
        break;
    case DayoSemantic::TLAS:
        result.accelerationStructure = bindings_->tlas;
        break;
    case DayoSemantic::Model2Mat:
        result.buffer = bindings_->modelToMaterial;
        break;
    case DayoSemantic::Mat2Model:
        result.buffer = bindings_->materialToModel;
        break;
    case DayoSemantic::Peekaboo:
        result.buffer = bindings_->peekaboo;
        break;
    case DayoSemantic::MatSelected:
        result.buffer = bindings_->materialSelected;
        break;
    case DayoSemantic::Skybox:
        result.texture = bindings_->skybox;
        break;
    case DayoSemantic::Skywalker:
        result.buffer = bindings_->skywalker;
        break;
    case DayoSemantic::SkywalkerRow:
        result.buffer = bindings_->skywalkerRow;
        break;
    case DayoSemantic::SkyboxSH:
        result.buffer = bindings_->skyboxSh;
        break;
    case DayoSemantic::ScreenBMP:
        result.texture = bindings_->screenBmp;
        break;
    case DayoSemantic::CloneCount:
        result.buffer = bindings_->cloneCount;
        break;
    case DayoSemantic::ScreenTexture:
        result.texture = bindings_->screenTexture;
        break;
    case DayoSemantic::ViewCB:
        result.buffer = bindings_->viewConstants;
        break;
    case DayoSemantic::ControllerCB:
        result.buffer = bindings_->controllerConstants;
        break;
    case DayoSemantic::TextureTable:
        result.buffer = bindings_->textureTable;
        break;
    case DayoSemantic::CBuff1:
        result.buffer = bindings_->passConstants;
        break;
    }
    return result.valid() ? std::optional<DayoResourceBinding>{result} : std::nullopt;
}

std::optional<DayoResourceBinding> DayoHostResourceProvider::resolve(std::string_view semantic) const noexcept {
    const auto parsed = dayoSemanticFromString(semantic);
    return parsed.has_value() ? resolve(*parsed) : std::nullopt;
}

bool DayoHostResourceProvider::require(std::span<const DayoSemantic> semantics, std::string* error) const {
    if (error != nullptr)
        error->clear();
    const auto missing =
        std::ranges::find_if(semantics, [this](DayoSemantic semantic) { return !resolve(semantic).has_value(); });
    if (missing == semantics.end())
        return true;
    setError(error, "missing real upstream host resource: " + std::string(toString(*missing)));
    return false;
}

} // namespace dayo::graphics
