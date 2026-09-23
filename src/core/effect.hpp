#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dayo::core::fx {
enum class RasterModelTarget : std::uint8_t;
}

namespace dayo::core {

enum class EffectPassType { rasterizer, postprocess, compute, raytracing, copy, clear, mipmap, oidn, unknown };

enum class FxFilter : std::uint8_t { point, linear, anisotropic };
enum class FxAddressMode : std::uint8_t { wrap, mirror, clamp, border };
enum class FxCompareOp : std::uint8_t {
    never,
    less,
    equal,
    lessEqual,
    greater,
    notEqual,
    greaterEqual,
    always,
};
enum class FxStencilOp : std::uint8_t {
    keep,
    zero,
    replace,
    incrementClamp,
    decrementClamp,
    invert,
    incrementWrap,
    decrementWrap
};
enum class FxLogicOp : std::uint8_t {
    clear,
    andOp,
    andReverse,
    copy,
    andInverted,
    noOp,
    xorOp,
    orOp,
    nor,
    equivalence,
    invert,
    orReverse,
    copyInverted,
    orInverted,
    nand,
    set,
};
enum class FxBorderColor : std::uint8_t { transparentBlack, opaqueBlack, opaqueWhite };

enum class EffectRasterSource : std::uint8_t { scene, buffer, vertexBufferless };
enum class EffectVertexInputRate : std::uint8_t { vertex, instance };
enum class EffectVertexFormat : std::uint8_t { r32Float, r32g32Float, r32g32b32Float, r32g32b32a32Float };
enum class EffectFunctionalPassKind : std::uint8_t { none, copy, clearRtv, clearUav, mipmapGen };

struct EffectSize {
    std::string base;
    bool absolute{};
    std::uint32_t width{1};
    std::uint32_t height{1};
    std::uint32_t depth{1};
    std::uint32_t dimension{};
    float widthRatio{1.0F};
    float heightRatio{1.0F};
    float depthRatio{1.0F};
    std::string convX{"x"};
    std::string convY{"y"};
    std::string convZ{"z"};
    std::string rounding{"trunc"};
};

struct EffectTexture {
    std::string name;
    std::string format;
    std::string view;
    std::string filename;
    std::string shared;
    bool mipmap{};
    EffectSize size;
    float widthRatio{1.0F};
    float heightRatio{1.0F};
    std::vector<std::string> conditions;
};

struct EffectBuffer {
    std::string name;
    std::string type;
    std::string format;
    std::string view;
    std::string shared;
    std::uint32_t elementSize{};
    EffectSize size;
    std::vector<std::string> conditions;
};

struct EffectSlider {
    float minimum{};
    float maximum{1.0F};
    float step{};
    float defaultValue{};
    bool logarithmic{};
    bool integer{};
};

struct EffectSampler {
    std::string name;
    std::string filter;
    std::string addressU{"WRAP"};
    std::string addressV{"WRAP"};
    std::string addressW{"WRAP"};
    FxFilter filterKind{FxFilter::linear};
    FxAddressMode addressModeU{FxAddressMode::wrap};
    FxAddressMode addressModeV{FxAddressMode::wrap};
    FxAddressMode addressModeW{FxAddressMode::wrap};
    float mipLodBias{};
    std::uint32_t maxAnisotropy{1};
    FxCompareOp comparisonFunc{FxCompareOp::always};
    FxBorderColor borderColor{FxBorderColor::transparentBlack};
    float minLod{};
    float maxLod{std::numeric_limits<float>::max()};
};

struct EffectClearValue {
    std::array<float, 4> color{};
    float depth{1.0F};
    std::uint32_t stencil{};
};

struct EffectAttachment {
    std::string name;
    bool clear{};
    EffectClearValue clearValue;
};

enum class EffectCullMode : std::uint8_t { none, front, back };
enum class EffectFrontFace : std::uint8_t { counterClockwise, clockwise };

enum class EffectDepthFunc : std::uint8_t {
    never,
    less,
    equal,
    lessEqual,
    greater,
    notEqual,
    greaterEqual,
    always,
};

struct EffectRasterizerState {
    EffectCullMode cullMode{EffectCullMode::back};
    EffectFrontFace frontFace{EffectFrontFace::counterClockwise};
};

struct EffectDepthStencilState {
    bool depthEnable{true};
    bool depthWrite{true};
    EffectDepthFunc depthFunc{EffectDepthFunc::less};
    bool stencilEnable{};
    std::uint32_t stencilReadMask{0xFFU};
    std::uint32_t stencilWriteMask{0xFFU};
    struct StencilFace {
        FxStencilOp fail{FxStencilOp::keep};
        FxStencilOp pass{FxStencilOp::keep};
        FxStencilOp depthFail{FxStencilOp::keep};
        FxCompareOp compare{FxCompareOp::always};
    } front, back;
};

struct EffectBlendAttachmentState {
    bool enabled{};
    std::string srcColor;
    std::string dstColor;
    std::string colorOp;
    std::string srcAlpha;
    std::string dstAlpha;
    std::string alphaOp;
    std::uint8_t colorWriteMask{0x0FU};
};

struct EffectBlendState {
    bool alphaToCoverage{};
    bool independentBlend{};
    bool logicOpEnable{};
    FxLogicOp logicOp{FxLogicOp::copy};
    std::array<EffectBlendAttachmentState, 8> targets{};
    std::uint32_t targetCount{};
};

struct EffectVertexBinding {
    std::uint32_t binding{};
    std::uint32_t stride{};
    EffectVertexInputRate rate{EffectVertexInputRate::vertex};
};

struct EffectVertexAttribute {
    std::uint32_t location{};
    std::uint32_t binding{};
    EffectVertexFormat format{EffectVertexFormat::r32g32b32Float};
    std::uint32_t offset{};
    std::string semanticName;
    std::uint32_t semanticIndex{};
    std::string formatName;
};

struct EffectVertexLayout {
    std::vector<EffectVertexBinding> bindings;
    std::vector<EffectVertexAttribute> attributes;
};

struct EffectGraphicsState {
    EffectRasterizerState rasterizer;
    EffectDepthStencilState depthStencil;
    std::vector<EffectBlendAttachmentState> blend;
    fx::RasterModelTarget modelTarget{static_cast<fx::RasterModelTarget>(0)};
    bool alphaToCoverage{};
    bool independentBlend{};
    bool logicOpEnable{};
    FxLogicOp logicOp{FxLogicOp::copy};
};

struct EffectHitGroup {
    std::string type;
    std::string closestHit;
    std::string anyHit;
    std::string intersection;
};

struct EffectFunctionalDispatch {
    EffectFunctionalPassKind kind{EffectFunctionalPassKind::none};
    std::string source;
    std::string destination;
    std::string target;
    EffectClearValue clearValue;
};

struct EffectPass {
    std::string name;
    EffectPassType type{EffectPassType::unknown};
    std::string vertexShader;
    std::string pixelShader;
    std::string computeShader;
    std::string rayGenerationShader;
    std::vector<std::string> missShaders;
    std::vector<EffectHitGroup> hitGroups;
    std::vector<std::string> callableShaders;
    std::vector<std::string> macros;
    std::array<std::uint32_t, 3> numThreads{};
    std::vector<std::string> conditions;
    std::vector<EffectAttachment> inputs;
    std::vector<EffectAttachment> renderTargets;
    std::vector<EffectAttachment> unorderedAccess;
    EffectAttachment depth;
    std::string rasterVertexBuffer;
    std::string rasterIndexBuffer;
    EffectGraphicsState graphics;
    // OIDN is a host operation rather than a shader pass. The explicit names
    // preserve the upstream operation even when the graph has no RTV/UAV
    // declaration for the host-owned output.
    std::string oidnInput;
    std::string oidnAlbedo;
    std::string oidnNormal;
    std::string oidnOutput;
    EffectSize outputSize;
    float outputWidthRatio{1.0F};
    float outputHeightRatio{1.0F};
    std::uint32_t maxPayloadSize{};
    std::uint32_t maxAttributeSize{};
    std::uint32_t maxRecursionDepth{1};
    EffectRasterSource rasterSource{EffectRasterSource::scene};
    EffectVertexLayout vertexLayout;
    EffectRasterizerState rasterizer;
    EffectDepthStencilState depthStencil;
    EffectBlendState blend;
    EffectFunctionalPassKind functionalKind{EffectFunctionalPassKind::none};
    EffectFunctionalDispatch functional;
};

struct EffectController {
    std::string name;
    std::string controllerName;
    std::string item;
    std::string type;
    std::optional<EffectSlider> slider{};
    std::string description{};
    std::vector<std::string> descriptions;
};

struct EffectMaterialDescriptor {
    std::string name;
    std::filesystem::path templatePath;
    std::filesystem::path defaultFile;
};

struct EffectGraph {
    std::filesystem::path sourcePath;
    std::string category;
    std::vector<EffectTexture> textures;
    std::vector<EffectTexture> textures3D;
    std::vector<EffectBuffer> buffers;
    std::vector<EffectSampler> samplers;
    std::vector<EffectController> controllers;
    std::vector<EffectPass> passes;
    std::optional<EffectMaterialDescriptor> materialDescriptor;
    std::uint32_t meshCloneCount{1};
    std::vector<std::string> memos;
    // MikuMikuDayo 1.30 allocates a 1024-byte implicit global constant buffer
    // unless an effect overrides this size.
    std::uint32_t globalVarSize{1024};
    bool globalVarSizeSpecified{};
    // The Jsonnet section is retained verbatim for diagnostics, round-trip
    // tooling, and upstream compatibility tests. Parsed fields remain the
    // execution ABI; this text prevents unknown fields from being silently
    // discarded at the first parse boundary.
    std::string rawYrzfx;
    std::string generatedCode;
    // HLSL text before [YRZFX] is part of the upstream shader source. It must
    // precede generated declarations because it defines Dayo/YRZ ABI types.
    std::string hlslPrefix;
    std::string hlsl;
};

using MaterialValue = std::variant<float, std::int32_t, bool, std::array<float, 2>, std::array<float, 3>,
                                   std::array<float, 4>, std::array<std::int32_t, 2>, std::array<std::int32_t, 3>,
                                   std::array<std::int32_t, 4>, std::filesystem::path, std::string>;

class MaterialParameterBlock {
  public:
    template <typename T> void set(std::string name, T value) {
        values_[std::move(name)] = std::move(value);
    }
    [[nodiscard]] const MaterialValue* find(std::string_view name) const noexcept;
    bool erase(std::string_view name);
    void clear() noexcept {
        values_.clear();
    }
    [[nodiscard]] const auto& values() const noexcept {
        return values_;
    }

  private:
    std::unordered_map<std::string, MaterialValue> values_;
};

struct EffectResourceBinding {
    std::string resource;
    bool write{};
};

struct CompiledPass {
    std::string name;
    EffectPassType type{EffectPassType::unknown};
    std::vector<EffectResourceBinding> resources;
    std::vector<std::string> barriers;
    std::string vertexShader;
    std::string pixelShader;
    std::string computeShader;
    std::string rayGenerationShader;
    std::vector<std::string> missShaders;
    std::vector<EffectHitGroup> hitGroups;
    std::vector<std::string> callableShaders;
    std::vector<std::string> conditions;
    float outputWidthRatio{1.0F};
    float outputHeightRatio{1.0F};
    std::uint32_t maxPayloadSize{};
    std::uint32_t maxAttributeSize{};
    std::uint32_t maxRecursionDepth{1};
    std::array<std::uint32_t, 3> numThreads{};
    EffectSize outputSize;
    EffectFunctionalPassKind functionalKind{EffectFunctionalPassKind::none};
    EffectFunctionalDispatch functional;
};

struct CompiledEffect {
    EffectGraph source;
    std::vector<CompiledPass> passes;
};

[[nodiscard]] CompiledEffect compileEffectGraph(const EffectGraph& graph);

struct EffectExecutionStats {
    std::size_t rasterPasses{};
    std::size_t computePasses{};
    std::size_t rayTracingPasses{};
    std::size_t oidnPasses{};
    std::size_t barriers{};
};

// Backend-neutral pass executor. Vulkan supplies the callback that turns a
// compiled pass into command-buffer work; this layer owns ordering and makes
// resource hazards visible to validation/tests.
class EffectExecutor {
  public:
    using PassCallback = std::function<void(const CompiledPass&)>;
    [[nodiscard]] EffectExecutionStats execute(const CompiledEffect& effect, const PassCallback& callback) const;
};

class EffectHotReloader {
  public:
    explicit EffectHotReloader(std::filesystem::path path);
    [[nodiscard]] const EffectGraph* current() const noexcept {
        return graph_ ? &*graph_ : nullptr;
    }
    // Reload is transactional: a compile/parse error leaves the last good graph active.
    bool poll(std::string* error = nullptr);
    [[nodiscard]] bool dirty() const noexcept {
        return dirty_;
    }

  private:
    std::filesystem::path path_;
    std::filesystem::file_time_type timestamp_{};
    std::optional<EffectGraph> graph_;
    bool dirty_{};
};

[[nodiscard]] EffectGraph loadEffectGraph(const std::filesystem::path& path);
// Parses an already loaded source buffer. This is the hot-reload entry point;
// callers must not be forced back to the stale on-disk version.
[[nodiscard]] EffectGraph loadEffectGraphFromText(const std::filesystem::path& path, std::string_view source);
[[nodiscard]] const char* toString(EffectPassType type) noexcept;

} // namespace dayo::core
