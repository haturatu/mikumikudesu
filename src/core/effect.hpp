#pragma once

#include <filesystem>
#include <array>
#include <cstdint>
#include <optional>
#include <memory>
#include <variant>
#include <unordered_map>
#include <string>
#include <string_view>
#include <functional>
#include <vector>

namespace dayo::core {

enum class EffectPassType { rasterizer, postprocess, compute, raytracing, unknown };

struct EffectTexture {
    std::string name;
    std::string format;
    std::string view;
    float widthRatio { 1.0F };
    float heightRatio { 1.0F };
};

struct EffectSampler {
    std::string name;
    std::string filter;
    std::string addressU { "WRAP" };
    std::string addressV { "WRAP" };
};

struct EffectAttachment {
    std::string name;
    bool clear {};
};

struct EffectPass {
    std::string name;
    EffectPassType type { EffectPassType::unknown };
    std::string vertexShader;
    std::string pixelShader;
    std::string computeShader;
    std::string rayGenerationShader;
    std::vector<std::string> missShaders;
    std::vector<std::string> macros;
    std::vector<std::string> conditions;
    std::vector<EffectAttachment> renderTargets;
    std::vector<EffectAttachment> unorderedAccess;
    EffectAttachment depth;
};

struct EffectGraph {
    std::filesystem::path sourcePath;
    std::string category;
    std::vector<EffectTexture> textures;
    std::vector<EffectSampler> samplers;
    std::vector<EffectPass> passes;
    std::string generatedCode;
    std::string hlsl;
};

using MaterialValue = std::variant<float, std::int32_t, bool,
                                   std::array<float, 2>, std::array<float, 3>, std::array<float, 4>,
                                   std::filesystem::path>;

class MaterialParameterBlock {
public:
    template <typename T>
    void set(std::string name, T value) { values_[std::move(name)] = std::move(value); }
    [[nodiscard]] const MaterialValue* find(std::string_view name) const noexcept;
    bool erase(std::string_view name);
    void clear() noexcept { values_.clear(); }
    [[nodiscard]] const auto& values() const noexcept { return values_; }

private:
    std::unordered_map<std::string, MaterialValue> values_;
};

struct EffectResourceBinding {
    std::string resource;
    bool write {};
};

struct CompiledPass {
    std::string name;
    EffectPassType type { EffectPassType::unknown };
    std::vector<EffectResourceBinding> resources;
    std::vector<std::string> barriers;
};

struct CompiledEffect {
    EffectGraph source;
    std::vector<CompiledPass> passes;
};

[[nodiscard]] CompiledEffect compileEffectGraph(const EffectGraph& graph);

struct EffectExecutionStats {
    std::size_t rasterPasses {};
    std::size_t computePasses {};
    std::size_t rayTracingPasses {};
    std::size_t barriers {};
};

// Backend-neutral pass executor. Vulkan supplies the callback that turns a
// compiled pass into command-buffer work; this layer owns ordering and makes
// resource hazards visible to validation/tests.
class EffectExecutor {
public:
    using PassCallback = std::function<void(const CompiledPass&)>;
    [[nodiscard]] EffectExecutionStats execute(const CompiledEffect& effect,
                                               const PassCallback& callback) const;
};

class EffectHotReloader {
public:
    explicit EffectHotReloader(std::filesystem::path path);
    [[nodiscard]] const EffectGraph* current() const noexcept { return graph_ ? &*graph_ : nullptr; }
    // Reload is transactional: a compile/parse error leaves the last good graph active.
    bool poll(std::string* error = nullptr);
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

private:
    std::filesystem::path path_;
    std::filesystem::file_time_type timestamp_ {};
    std::optional<EffectGraph> graph_;
    bool dirty_ {};
};

[[nodiscard]] EffectGraph loadEffectGraph(const std::filesystem::path& path);
[[nodiscard]] const char* toString(EffectPassType type) noexcept;

} // namespace dayo::core
