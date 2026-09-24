#include "graphics/fx_material_scene_runtime.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dayo::graphics {
namespace {

constexpr std::uintmax_t kMaxAnnotationBytes = 16U * 1024U * 1024U;
constexpr auto kAnnotationScanInterval = std::chrono::milliseconds{500};

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] bool isInlineAnnotation(std::string_view value) noexcept {
    if (value.find_first_of("\r\n") != std::string_view::npos)
        return true;
    const auto colon = value.find(':');
    if (colon == std::string_view::npos)
        return false;
    if (colon == 1 && std::isalpha(static_cast<unsigned char>(value.front())) != 0 && value.size() > 2 &&
        (value[2] == '/' || value[2] == '\\'))
        return false;
    return true;
}

[[nodiscard]] bool isFileBackedTexture(const core::fx::MaterialBindingPlan::ResolvedTextureField& texture) {
    if (texture.path.empty() || core::fx::isScreenBmpToken(texture.path))
        return false;
    const std::filesystem::path assigned(texture.path);
    const auto isRegularFile = [](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && !error;
    };
    if (assigned.is_absolute())
        return isRegularFile(assigned);
    if (!texture.baseDirectory.empty() && isRegularFile(texture.baseDirectory / assigned))
        return true;
    return isRegularFile(assigned);
}

[[nodiscard]] bool sameSchema(const core::fx::MaterialTemplateSchema& left,
                              const core::fx::MaterialTemplateSchema& right) {
    const auto sameFields = std::ranges::equal(left.fields, right.fields, [](const auto& lhs, const auto& rhs) {
        return lhs.name == rhs.name && lhs.type == rhs.type && lhs.components == rhs.components;
    });
    const auto sameTextures = std::ranges::equal(left.textures, right.textures, [](const auto& lhs, const auto& rhs) {
        return lhs.name == rhs.name && lhs.dimension == rhs.dimension && lhs.index == rhs.index &&
               lhs.mipmapped == rhs.mipmapped;
    });
    const auto sameEnums = std::ranges::equal(left.enums, right.enums, [](const auto& lhs, const auto& rhs) {
        return lhs.field == rhs.field &&
               std::ranges::equal(lhs.values, rhs.values, [](const auto& leftValue, const auto& rightValue) {
                   return leftValue.name == rightValue.name && leftValue.value == rightValue.value;
               });
    });
    const auto sameTextureAssignments = std::ranges::equal(
        left.templateTextureAssignments, right.templateTextureAssignments, [](const auto& lhs, const auto& rhs) {
            return lhs.field == rhs.field && lhs.dimension == rhs.dimension && lhs.index == rhs.index &&
                   lhs.mipmapped == rhs.mipmapped && lhs.path == rhs.path && lhs.baseDirectory == rhs.baseDirectory;
        });
    const auto sameAnnotations = [&left, &right]() {
        const auto sameValues = std::ranges::equal(
            left.defaultFileAnnotation.values, right.defaultFileAnnotation.values,
            [](const auto& lhs, const auto& rhs) { return lhs.field == rhs.field && lhs.source == rhs.source; });
        const auto sameAnnotationTextures =
            std::ranges::equal(left.defaultFileAnnotation.textures, right.defaultFileAnnotation.textures,
                               [](const auto& lhs, const auto& rhs) {
                                   return lhs.field == rhs.field && lhs.dimension == rhs.dimension &&
                                          lhs.index == rhs.index && lhs.mipmapped == rhs.mipmapped &&
                                          lhs.path == rhs.path && lhs.baseDirectory == rhs.baseDirectory;
                               });
        return sameValues && sameAnnotationTextures;
    }();
    return left.name == right.name && left.sourceText == right.sourceText &&
           left.defaultFileSourceText == right.defaultFileSourceText &&
           left.templateTextureBaseDirectory == right.templateTextureBaseDirectory &&
           left.defaultFileBaseDirectory == right.defaultFileBaseDirectory &&
           left.defaults.values() == right.defaults.values() && sameFields && sameTextures && sameEnums &&
           sameTextureAssignments && sameAnnotations;
}

[[nodiscard]] std::int64_t checkedInteger(std::size_t value, std::string_view name) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("MatDesc evaluation " + std::string(name) + " exceeds signed 64-bit range");
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::int64_t checkedFrameIndex(float frame) {
    if (!std::isfinite(frame) ||
        static_cast<double>(frame) < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        static_cast<double>(frame) > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("MatDesc evaluation frame exceeds signed 64-bit range");
    return static_cast<std::int64_t>(frame);
}

[[nodiscard]] core::fx::FxEvalContext evaluationContext(const fx::FxFrameContext& frame,
                                                        const FxMaterialSceneModel& model) {
    if (frame.sample > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("MatDesc evaluation sample exceeds signed 64-bit range");
    const auto cloneCount = std::max(model.cloneCount, 1U);
    if (cloneCount != 0 && model.vertexCount > std::numeric_limits<std::size_t>::max() / cloneCount)
        throw std::overflow_error("MatDesc cloned vertex count overflow");
    core::fx::FxEvalContext result;
    result.rtWidth = frame.renderWidth;
    result.rtHeight = frame.renderHeight;
    result.vertexCount = checkedInteger(model.vertexCount, "vertex count");
    result.totalMaterial = checkedInteger(frame.totalMaterial, "material count");
    result.modelIndex = model.modelIndex;
    result.cloneCount = cloneCount;
    result.clonedVertexCount =
        checkedInteger(model.vertexCount * static_cast<std::size_t>(cloneCount), "cloned vertex count");
    result.frameIndex = checkedFrameIndex(frame.frame);
    result.sampleIndex = static_cast<std::int64_t>(frame.sample);
    result.time = frame.time;
    result.namedSymbols = frame.expressionSymbols;
    return result;
}

[[nodiscard]] std::string readAnnotationFile(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto expectedSize = std::filesystem::file_size(path, sizeError);
    if (sizeError)
        throw std::runtime_error("cannot stat MatDesc annotation: " + path.string());
    if (expectedSize > kMaxAnnotationBytes)
        throw std::length_error("MatDesc annotation exceeds 16 MiB: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open MatDesc annotation: " + path.string());
    std::string result{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad())
        throw std::runtime_error("failed while reading MatDesc annotation: " + path.string());
    if (result.size() > kMaxAnnotationBytes)
        throw std::length_error("MatDesc annotation exceeds 16 MiB: " + path.string());
    return result;
}

[[nodiscard]] std::uint64_t annotationHash(std::string_view text) noexcept {
    // FNV-1a lets periodic scans detect same-size edits even on filesystems
    // whose timestamp resolution is too coarse to distinguish them.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

FxMaterialSceneRuntime::AnnotationSource
FxMaterialSceneRuntime::resolveAnnotation(const FxMaterialSceneModel& model,
                                          const core::MaterialEditorState& material) const {
    AnnotationSource result;
    result.original = material.annotation.generic_string();
    if (result.original.empty())
        return result;

    const std::filesystem::path raw(result.original);
    std::vector<std::filesystem::path> candidates;
    if (raw.is_absolute()) {
        candidates.push_back(raw);
    } else {
        if (!model.sourcePath.empty())
            candidates.push_back(model.sourcePath.parent_path() / raw);
        if (!model.projectDirectory.empty())
            candidates.push_back(model.projectDirectory / raw);
        candidates.push_back(raw);
    }
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error)
            continue;
        result.kind = AnnotationKind::file;
        result.path = std::filesystem::absolute(candidate).lexically_normal();
        result.baseDirectory = result.path.parent_path();
        result.contentHash = annotationHash(readAnnotationFile(result.path));
        return result;
    }

    if (isInlineAnnotation(result.original)) {
        result.kind = AnnotationKind::inlineText;
        result.text = result.original;
        result.baseDirectory = model.sourcePath.parent_path();
    }
    return result;
}

bool FxMaterialSceneRuntime::matches(const core::fx::MaterialTemplateSchema& schema,
                                     std::span<const FxMaterialSceneModel> models) {
    if (!hasSchema_ || !sameSchema(schema, schemaSnapshot_) || models.size() != models_.size())
        return false;
    for (std::size_t modelIndex = 0; modelIndex < models.size(); ++modelIndex) {
        const auto& input = models[modelIndex];
        const auto& cached = models_[modelIndex];
        if (input.id != cached.id || input.sourcePath != cached.sourcePath ||
            input.projectDirectory != cached.projectDirectory || input.modelIndex != cached.modelIndex ||
            input.vertexCount != cached.vertexCount || std::max(input.cloneCount, 1U) != cached.cloneCount ||
            input.materials.size() != cached.materials.size())
            return false;
        for (std::size_t materialIndex = 0; materialIndex < input.materials.size(); ++materialIndex) {
            const auto& source = input.materials[materialIndex];
            const auto& material = cached.materials[materialIndex];
            if (source.annotation.generic_string() != material.annotation.original ||
                source.parameters.values() != material.parameters.values())
                return false;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < nextAnnotationScan_)
        return true;

    nextAnnotationScan_ = now + kAnnotationScanInterval;
    try {
        for (std::size_t modelIndex = 0; modelIndex < models.size(); ++modelIndex) {
            const auto& input = models[modelIndex];
            const auto& cached = models_[modelIndex];
            for (std::size_t materialIndex = 0; materialIndex < input.materials.size(); ++materialIndex) {
                if (resolveAnnotation(input, input.materials[materialIndex]) !=
                    cached.materials[materialIndex].annotation)
                    return false;
                const auto& cachedMaterial = cached.materials[materialIndex];
                if (cachedMaterial.fileBackedTextures.size() != cachedMaterial.binding.orderedTextures.size())
                    return false;
                for (std::size_t textureIndex = 0; textureIndex < cachedMaterial.binding.orderedTextures.size();
                     ++textureIndex) {
                    if (isFileBackedTexture(cachedMaterial.binding.orderedTextures[textureIndex]) !=
                        cachedMaterial.fileBackedTextures[textureIndex])
                        return false;
                }
            }
        }
    } catch (...) {
        // Re-link through the normal error-reporting path if polling cannot
        // inspect a file (for example, a concurrent delete or permission change).
        return false;
    }
    return true;
}

bool FxMaterialSceneRuntime::link(const core::fx::MaterialTemplateSchema& schema,
                                  std::span<const FxMaterialSceneModel> models, std::string* error) {
    try {
        std::vector<CachedModel> linkedModels;
        linkedModels.reserve(models.size());
        for (const auto& input : models) {
            CachedModel linkedModel{.id = input.id,
                                    .sourcePath = input.sourcePath,
                                    .projectDirectory = input.projectDirectory,
                                    .modelIndex = input.modelIndex,
                                    .vertexCount = input.vertexCount,
                                    .cloneCount = std::max(input.cloneCount, 1U),
                                    .materials = {}};
            linkedModel.materials.reserve(input.materials.size());
            for (const auto& material : input.materials) {
                auto annotation = resolveAnnotation(input, material);
                core::fx::MaterialInstance instance;
                instance.templateName = schema.name;
                instance.overrides = material.parameters;
                if (annotation.kind == AnnotationKind::file) {
                    const auto source = readAnnotationFile(annotation.path);
                    instance.annotationOverrides =
                        core::fx::parseMaterialAnnotation(schema, source, annotation.baseDirectory);
                } else if (annotation.kind == AnnotationKind::inlineText) {
                    instance.annotationOverrides =
                        core::fx::parseMaterialAnnotation(schema, annotation.text, annotation.baseDirectory);
                } else if (!annotation.original.empty()) {
                    log::warn("MatDesc annotation file was not found; using template defaults: ", annotation.original);
                }
                auto binding = core::fx::linkMaterial(schema, &instance);
                std::vector<bool> fileBackedTextures;
                fileBackedTextures.reserve(binding.orderedTextures.size());
                for (const auto& texture : binding.orderedTextures)
                    fileBackedTextures.push_back(isFileBackedTexture(texture));
                linkedModel.materials.push_back({.parameters = material.parameters,
                                                 .annotation = std::move(annotation),
                                                 .instance = std::move(instance),
                                                 .binding = std::move(binding),
                                                 .fileBackedTextures = std::move(fileBackedTextures)});
            }
            linkedModels.push_back(std::move(linkedModel));
        }
        schemaSnapshot_ = schema;
        models_ = std::move(linkedModels);
        nextAnnotationScan_ = std::chrono::steady_clock::now() + kAnnotationScanInterval;
        hasSchema_ = true;
        return true;
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        return false;
    } catch (...) {
        setError(error, "MatDesc scene instance linking failed");
        return false;
    }
}

bool FxMaterialSceneRuntime::sync(Device& device, const core::fx::MaterialTemplateSchema& schema,
                                  std::span<const FxMaterialSceneModel> models, const fx::FxFrameContext& context,
                                  std::string* error, const FxMaterialTextureResolver& textureResolver) {
    if (error != nullptr)
        error->clear();
    descriptorLayoutChanged_ = false;
    if (!matches(schema, models) && !link(schema, models, error))
        return false;
    try {
        std::size_t totalMaterials = 0;
        for (const auto& model : models_) {
            if (model.materials.size() > std::numeric_limits<std::size_t>::max() - totalMaterials)
                throw std::overflow_error("MatDesc scene material count overflow");
            totalMaterials += model.materials.size();
        }
        std::vector<core::fx::EvaluatedMaterialBinding> evaluated;
        std::vector<core::fx::MaterialBindingPlan> frameBindings;
        std::vector<core::fx::MaterialGpuTableMaterial> materialRefs;
        std::vector<core::fx::MaterialGpuTableModel> tableModels;
        evaluated.reserve(totalMaterials);
        frameBindings.reserve(totalMaterials);
        materialRefs.reserve(totalMaterials);
        tableModels.reserve(models_.size());
        std::map<std::string, FxMaterialExternalTexture> externalTextureMap;
        for (std::size_t modelIndex = 0; modelIndex < models_.size(); ++modelIndex) {
            const auto& model = models_[modelIndex];
            const auto start = materialRefs.size();
            const auto& input = models[modelIndex];
            if (input.id != model.id || input.modelIndex != model.modelIndex)
                throw std::logic_error("MatDesc scene models changed order while the table was being evaluated");
            const auto evalContext = evaluationContext(context, input);
            for (std::size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex) {
                const auto& material = model.materials[materialIndex];
                auto binding = material.binding;
                for (std::size_t textureIndex = 0; textureIndex < binding.orderedTextures.size(); ++textureIndex) {
                    auto& texture = binding.orderedTextures[textureIndex];
                    if (texture.path.empty() || !texture.physicalTextureIndex.has_value())
                        continue;
                    std::optional<FxMaterialExternalTexture> external;
                    if (textureResolver)
                        external = textureResolver(model.id, texture.schema, texture.path);
                    if (external.has_value()) {
                        if (external->identity.empty() || !external->texture.valid() ||
                            external->dimension != texture.schema.dimension)
                            throw std::invalid_argument(
                                "MatDesc texture resolver returned an invalid external texture: " + texture.path);
                        auto& descriptor = binding.layout.uniqueTextures.at(*texture.physicalTextureIndex);
                        descriptor.externalId = external->identity;
                        descriptor.path.clear();
                        const auto [found, inserted] = externalTextureMap.emplace(external->identity, *external);
                        if (!inserted && (found->second.texture != external->texture ||
                                          found->second.dimension != external->dimension ||
                                          found->second.generation != external->generation))
                            throw std::logic_error("MatDesc external texture identity resolved inconsistently: " +
                                                   external->identity);
                    } else if (core::fx::isScreenBmpToken(texture.path) ||
                               !material.fileBackedTextures.at(textureIndex)) {
                        // Resolve host/deformer resources before considering a file. Unknown symbols and missing
                        // files intentionally map to the dimension-correct fallback instead of a guessed path.
                        texture.physicalTextureIndex.reset();
                    }
                }
                evaluated.push_back(core::fx::evaluateMaterialValues(binding, evalContext));
                frameBindings.push_back(std::move(binding));
                materialRefs.push_back({.binding = &frameBindings.back(), .evaluated = &evaluated.back()});
            }
            tableModels.push_back({.materials = std::span<const core::fx::MaterialGpuTableMaterial>(materialRefs)
                                                    .subspan(start, model.materials.size())});
        }
        const auto table = core::fx::makeMaterialGpuTableData(schema, tableModels);
        std::vector<FxMaterialExternalTexture> externalTextures;
        externalTextures.reserve(externalTextureMap.size());
        for (auto& [identity, texture] : externalTextureMap) {
            static_cast<void>(identity);
            externalTextures.push_back(std::move(texture));
        }
        const bool wasReady = gpuRuntime_.ready();
        const auto previousBindings = gpuRuntime_.bindings();
        const auto previous2DCount = previousBindings.textures2D.size();
        const auto previous3DCount = previousBindings.textures3D.size();
        if (!gpuRuntime_.sync(device, table, error, externalTextures))
            return false;
        const auto currentBindings = gpuRuntime_.bindings();
        descriptorLayoutChanged_ = wasReady && (previous2DCount != currentBindings.textures2D.size() ||
                                                previous3DCount != currentBindings.textures3D.size());
        return true;
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        return false;
    } catch (...) {
        setError(error, "MatDesc scene table synchronization failed");
        return false;
    }
}

void FxMaterialSceneRuntime::reset() noexcept {
    gpuRuntime_.reset();
    schemaSnapshot_ = {};
    models_.clear();
    nextAnnotationScan_ = {};
    hasSchema_ = false;
    descriptorLayoutChanged_ = false;
}

} // namespace dayo::graphics
