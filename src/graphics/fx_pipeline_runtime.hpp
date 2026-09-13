#pragma once

#include "fx/fx_compiler.hpp"
#include "fx/fx_shader_cache.hpp"
#include "fx/fx_shader_compiler.hpp"
#include "graphics/device.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dayo::graphics {

// Materializes a compiled FX program into typed Vulkan shader/pipeline/SBT
// handles. Descriptor and push-constant layouts remain owned by the renderer
// because the FX graph cannot infer application-specific resource bindings.
class FxPipelineRuntime {
  public:
    using LayoutResolver =
        std::function<std::optional<handles::PipelineLayoutHandle>(const fx::FxDispatch&)>;

    FxPipelineRuntime() = default;
    ~FxPipelineRuntime();

    FxPipelineRuntime(const FxPipelineRuntime&) = delete;
    FxPipelineRuntime& operator=(const FxPipelineRuntime&) = delete;

    [[nodiscard]] bool build(Device& device, const fx::FxProgram& program, const fx::FxShaderCompiler& compiler,
                              const LayoutResolver& resolveLayout, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] std::optional<handles::PipelineHandle> resolvePipeline(const fx::FxDispatch& dispatch) const;
    [[nodiscard]] std::optional<handles::ShaderBindingTableHandle>
    resolveShaderBindingTable(const fx::FxDispatch& dispatch) const;
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

  private:
    struct Entry {
        handles::PipelineHandle pipeline{};
        handles::ShaderBindingTableHandle sbt{};
        std::vector<handles::ShaderHandle> shaders;
    };

    [[nodiscard]] handles::ShaderHandle compileShader(Device& device, const fx::FxProgram& program,
                                                      const fx::FxDispatch& dispatch, std::string_view entryPoint,
                                                      fx::FxShaderStage stage, const fx::FxShaderCompiler& compiler,
                                                      Entry& entry);
    void destroyEntry(const Entry& entry) noexcept;

    Device* device_{nullptr};
    std::unordered_map<std::string, Entry> entries_;
    fx::FxShaderCache shaderCache_;
};

} // namespace dayo::graphics
