#pragma once

#include "core/effect.hpp"
#include "core/fx/fx_expr.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dayo::core::fx {

enum class MaterialFieldType : std::uint8_t { floatingPoint, signedInteger };
enum class MaterialTextureDimension : std::uint8_t { twoD, threeD };

struct MaterialFieldSchema {
    std::string name;
    MaterialFieldType type{MaterialFieldType::floatingPoint};
    std::uint32_t components{1};
};

struct MaterialTextureSchema {
    std::string name;
    MaterialTextureDimension dimension{MaterialTextureDimension::twoD};
    std::uint32_t index{};
    bool mipmapped{};
};

struct MaterialEnumValue {
    std::string name;
    std::int32_t value{};
};

struct MaterialEnumSchema {
    std::string field;
    std::vector<MaterialEnumValue> values;
};

struct MaterialValueExpression {
    std::string field;
    std::string source;
    std::vector<FxExpr> components;
};

struct MaterialTextureAssignment {
    std::string field;
    MaterialTextureDimension dimension{MaterialTextureDimension::twoD};
    std::uint32_t index{};
    bool mipmapped{};
    std::string path;
    std::filesystem::path baseDirectory;
};

struct MaterialAnnotation {
    std::vector<MaterialValueExpression> values;
    std::vector<MaterialTextureAssignment> textures;
};

// Ordered schema for an upstream MatDesc template. Template defaults remain
// concrete (upstream disallows expressions in templates); a defaultFile is a
// separate annotation whose expressions and texture paths are retained for
// invocation-time evaluation and resource linking.
struct MaterialTemplateSchema {
    std::string name;
    std::vector<MaterialFieldSchema> fields;
    std::vector<MaterialTextureSchema> textures;
    std::vector<MaterialEnumSchema> enums;
    MaterialParameterBlock defaults;
    std::vector<MaterialTextureAssignment> templateTextureAssignments;
    MaterialAnnotation defaultFileAnnotation;
    std::filesystem::path templateTextureBaseDirectory;
    std::filesystem::path defaultFileBaseDirectory;
    std::string sourceText;
    std::string defaultFileSourceText;
};

[[nodiscard]] MaterialTemplateSchema parseMaterialTemplateSchema(std::string_view source, std::string name = {});
[[nodiscard]] MaterialTemplateSchema loadMaterialTemplateSchema(const std::filesystem::path& path,
                                                                std::string name = {},
                                                                std::filesystem::path textureBaseDirectory = {});
[[nodiscard]] MaterialAnnotation parseMaterialAnnotation(const MaterialTemplateSchema& schema, std::string_view source,
                                                         std::filesystem::path baseDirectory = {});
void applyMaterialDefaultFile(MaterialTemplateSchema& schema, std::string_view source,
                              std::filesystem::path baseDirectory = {});
void loadMaterialDefaultFile(MaterialTemplateSchema& schema, const std::filesystem::path& path);

// Linker-only material layer built on top of MaterialParameterBlock.
// No GPU work happens here: this layer folds resource aliases into
// canonical IDs, deduplicates textures, and assigns deterministic _R slots.

// Canonical texture identity includes the YRZFX texture view contract.
struct MaterialTextureDesc {
    std::string path;
    std::string format;
    std::string colorspace;
    std::string mipPolicy;
    MaterialTextureDimension dimension{MaterialTextureDimension::twoD};
    bool mipmapped{};
};

struct MaterialTextureKey {
    std::string path;
    std::string format;
    std::string colorspace;
    std::string mipPolicy;
    MaterialTextureDimension dimension{MaterialTextureDimension::twoD};
    bool mipmapped{};

    bool operator==(const MaterialTextureKey& other) const noexcept {
        return path == other.path && format == other.format && colorspace == other.colorspace &&
               mipPolicy == other.mipPolicy && dimension == other.dimension && mipmapped == other.mipmapped;
    }
};

struct MaterialTextureKeyHash {
    [[nodiscard]] std::size_t operator()(const MaterialTextureKey& key) const noexcept;
};

[[nodiscard]] std::string normalizeTexturePath(std::string_view path);
[[nodiscard]] std::string normalizeTextureToken(std::string_view token) noexcept;
[[nodiscard]] MaterialTextureKey makeTextureKey(const MaterialTextureDesc& desc);
[[nodiscard]] std::string textureKeyString(const MaterialTextureKey& key);

// One declared resource. `ref` is an explicit alias to another id.
// `shared` puts the id in the cross-material shared namespace.
// `shareTags` merges every decl with the identical sorted tag set.
struct MaterialResourceDecl {
    std::string id;
    std::string ref;
    bool shared{};
    std::vector<std::string> shareTags;
    MaterialTextureDesc texture{};
    bool hasTexture{};
};

struct MaterialTemplate {
    std::string name;
    MaterialParameterBlock defaults;
    std::vector<MaterialResourceDecl> resources;
};

struct MaterialInstance {
    std::string templateName;
    MaterialParameterBlock overrides;
    MaterialAnnotation annotationOverrides;
    std::vector<MaterialResourceDecl> extraResources;
};

struct MaterialGpuLayout {
    // Sorted canonical ids, e.g. {"shared:albedo", "tag:base", "local"}.
    std::vector<std::string> canonicalResources;
    // Deduplicated textures sorted by canonical key string.
    std::vector<MaterialTextureDesc> uniqueTextures;
    // Local id -> canonical id.
    std::unordered_map<std::string, std::string> localToCanonical;
    // Canonical id -> deterministic slot ("_R0", "_R1", ...).
    std::unordered_map<std::string, std::string> canonicalToSlot;

    [[nodiscard]] std::string slotForLocal(std::string_view localId) const;
    [[nodiscard]] std::string slotForCanonical(std::string_view canonical) const;
    [[nodiscard]] bool hasLocal(std::string_view localId) const noexcept;
};

struct MaterialBindingPlan {
    MaterialGpuLayout layout;
    // Populated by the legacy MaterialTemplate overload. Schema-based plans
    // keep expressions linked until evaluateMaterialValues() is called.
    MaterialParameterBlock resolvedParameters;
    struct LinkedField {
        MaterialFieldSchema schema;
        MaterialValueExpression expression;
    };
    struct ResolvedTextureField {
        MaterialTextureSchema schema;
        std::string canonicalId;
        std::optional<std::size_t> physicalTextureIndex;
        std::string path;
        std::filesystem::path baseDirectory;
    };
    // Expressions and textures preserve HLSL declaration order/logical index.
    std::vector<LinkedField> orderedExpressions;
    std::vector<ResolvedTextureField> orderedTextures;

    [[nodiscard]] const std::string* slotFor(std::string_view localId) const noexcept;
};

struct EvaluatedMaterialBinding {
    MaterialParameterBlock values;
    // Values in the exact HLSL StructuredBuffer<FooValue> field declaration order.
    struct ResolvedField {
        MaterialFieldSchema schema;
        MaterialValue value;
    };
    std::vector<ResolvedField> orderedValues;
};

struct MaterialStructuredFieldLayout {
    MaterialFieldSchema schema;
    std::size_t offset{};
    std::size_t size{};
};

struct MaterialStructuredBufferLayout {
    // DXC storage-buffer member offsets and ArrayStride, matching the
    // -fvk-use-dx-layout option and the generated FooValue declaration.
    std::vector<MaterialStructuredFieldLayout> fields;
    std::size_t stride{};
};

struct MaterialStructuredBufferData {
    MaterialStructuredBufferLayout layout;
    std::vector<std::byte> bytes;
    std::size_t count{};
};

inline constexpr std::uint32_t kMissingMaterialTextureIndex = UINT32_MAX;

struct MaterialGpuTableMaterial {
    const MaterialBindingPlan* binding{};
    const EvaluatedMaterialBinding* evaluated{};
};

struct MaterialGpuTableModel {
    std::span<const MaterialGpuTableMaterial> materials;
};

// CPU upload payload matching the generated MatDesc HLSL accessors:
// _idx[model] + subID selects a value row, while _tex/_tex3D use the
// material row and the schema's explicit logical texture index.
struct MaterialGpuTableData {
    std::uint32_t textureSlotCount{};
    std::vector<std::uint32_t> materialIndices;
    std::vector<std::uint32_t> textureIndices2D;
    std::vector<std::uint32_t> textureIndices3D;
    std::vector<MaterialTextureDesc> textures2D;
    std::vector<MaterialTextureDesc> textures3D;
    MaterialStructuredBufferData values;
};

[[nodiscard]] MaterialStructuredBufferLayout makeMaterialStructuredBufferLayout(const MaterialTemplateSchema& schema);
[[nodiscard]] MaterialStructuredBufferData
packMaterialStructuredBuffer(const MaterialStructuredBufferLayout& layout,
                             std::span<const EvaluatedMaterialBinding> materials);
[[nodiscard]] MaterialGpuTableData makeMaterialGpuTableData(const MaterialTemplateSchema& schema,
                                                            std::span<const MaterialGpuTableModel> models);

// Alias folding rules (priority: ref > shareTags > shared > concrete):
// - ref="B"        -> canonical(resolve(B)); missing target keeps "B".
// - shareTags=[..] -> canonical("tag:" + sorted-normalized-tags joined by "+").
// - shared=true    -> canonical("shared:" + trimmed id).
// - otherwise      -> canonical(trimmed id).
// Cycles resolve to the lexicographically smallest id in the cycle.
[[nodiscard]] std::string resolveCanonicalResourceId(std::string_view id,
                                                     const std::unordered_map<std::string, MaterialResourceDecl>& byId);

[[nodiscard]] MaterialGpuLayout linkMaterialLayout(const MaterialTemplate& templ,
                                                   const MaterialInstance* instance = nullptr);
[[nodiscard]] MaterialBindingPlan linkMaterial(const MaterialTemplate& templ,
                                               const MaterialInstance* instance = nullptr);
[[nodiscard]] MaterialBindingPlan linkMaterial(const MaterialTemplateSchema& schema,
                                               const MaterialInstance* instance = nullptr);
[[nodiscard]] EvaluatedMaterialBinding evaluateMaterialValues(const MaterialBindingPlan& plan,
                                                              const FxEvalContext& context);

} // namespace dayo::core::fx
