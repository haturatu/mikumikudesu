#pragma once

#include "core/fx/fx_material.hpp"
#include "core/scene.hpp"
#include "fx/fx_frame.hpp"
#include "graphics/fx_material_gpu_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

struct FxMaterialSceneModel {
    core::ModelId id{};
    std::filesystem::path sourcePath;
    std::filesystem::path projectDirectory;
    std::uint32_t modelIndex{};
    std::size_t vertexCount{};
    std::uint32_t cloneCount{1};
    std::span<const core::MaterialEditorState> materials;
};

using FxMaterialTextureResolver = std::function<std::optional<FxMaterialExternalTexture>(
    core::ModelId, const core::fx::MaterialTextureSchema&, std::string_view)>;

// Links per-model MatDesc annotations once, evaluates their expressions for
// each frame, and uploads the resulting scene-wide tables and texture catalogs.
class FxMaterialSceneRuntime {
  public:
    FxMaterialSceneRuntime() = default;
    ~FxMaterialSceneRuntime() = default;

    FxMaterialSceneRuntime(const FxMaterialSceneRuntime&) = delete;
    FxMaterialSceneRuntime& operator=(const FxMaterialSceneRuntime&) = delete;

    [[nodiscard]] bool sync(Device& device, const core::fx::MaterialTemplateSchema& schema,
                            std::span<const FxMaterialSceneModel> models, const fx::FxFrameContext& context,
                            std::string* error = nullptr, const FxMaterialTextureResolver& textureResolver = {});
    void invalidateLinks() noexcept {
        hasSchema_ = false;
    }
    void reset() noexcept;

    [[nodiscard]] const FxMaterialGpuRuntime& gpuRuntime() const noexcept {
        return gpuRuntime_;
    }
    [[nodiscard]] bool descriptorLayoutChanged() const noexcept {
        return descriptorLayoutChanged_;
    }

  private:
    enum class AnnotationKind : std::uint8_t { none, file, inlineText };

    struct AnnotationSource {
        AnnotationKind kind{AnnotationKind::none};
        std::string original;
        std::filesystem::path path;
        std::filesystem::path baseDirectory;
        std::uint64_t contentHash{};
        std::string text;

        bool operator==(const AnnotationSource&) const = default;
    };

    struct CachedMaterial {
        core::MaterialParameterBlock parameters;
        AnnotationSource annotation;
        core::fx::MaterialInstance instance;
        core::fx::MaterialBindingPlan binding;
        std::vector<bool> fileBackedTextures;
    };

    struct CachedModel {
        core::ModelId id{};
        std::filesystem::path sourcePath;
        std::filesystem::path projectDirectory;
        std::uint32_t modelIndex{};
        std::size_t vertexCount{};
        std::uint32_t cloneCount{1};
        std::vector<CachedMaterial> materials;
    };

    [[nodiscard]] AnnotationSource resolveAnnotation(const FxMaterialSceneModel& model,
                                                     const core::MaterialEditorState& material) const;
    [[nodiscard]] bool matches(const core::fx::MaterialTemplateSchema& schema,
                               std::span<const FxMaterialSceneModel> models);
    [[nodiscard]] bool link(const core::fx::MaterialTemplateSchema& schema,
                            std::span<const FxMaterialSceneModel> models, std::string* error);

    FxMaterialGpuRuntime gpuRuntime_;
    core::fx::MaterialTemplateSchema schemaSnapshot_;
    std::vector<CachedModel> models_;
    std::chrono::steady_clock::time_point nextAnnotationScan_{};
    bool hasSchema_{};
    bool descriptorLayoutChanged_{};
};

} // namespace dayo::graphics
